#ifdef _WIN32
    // 必須在 windows.h 之前定義，避免 winsock 衝突
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0601
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
#endif

#include <windows.h>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cctype>
#include <ctime>
#include <system_error>
#include <string>
#include <chrono>
#include <atomic>
#include <filesystem>
#include <algorithm>
#include <functional>
#include <fstream>
#include <unordered_set>
#include "common/type.h"
#include "comport/comport.h"
#include "flash/flash.h"
#include "utils/talker/talker.h"
#include "utils/listener/listener.h"
#include "utils/zip/unzip.h"
#include "auto_flash/local_server.h"

#include <queue>
#include <set>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <thread>

std::queue<int> new_com_queue;
std::unordered_map<int, ComportInfo> comport_map;  // COM number -> ComportInfo
std::vector<int> current_coms;

std::mutex com_mutex;
std::condition_variable com_cv;

// File copy status
FileInfo global_file_info;
std::mutex file_info_mutex;

// bool auto_flash_status{false};
bool auto_flash{false};
bool prev_auto_flash{false};  // Track previous state

std::atomic<bool> copy_cancel{false};
// 上一次複製／解壓是不是因為「被取消」而結束。收斂保護只能對「真的跑完卻
// 沒有改善」的情況計數；把取消也算進去的話，使用者按兩次停止就會把這組
// installer/download 永久閂鎖住，之後再也不會複製（實測就是這樣卡在
// waiting_for_copy 不動）。
std::atomic<bool> last_copy_cancelled{false};

// 磁碟空間不足的提示。
//
// 使用者要求「空間不夠不要變成 FAIL/ERROR，用 warning 就好」，所以這個狀況不走
// COPY_FAILED；但 file-status 檢查每 2 秒就會依 validation 重算一次 warning_msg，
// 光在 copy_worker 裡寫一次會立刻被蓋掉。因此把它存在這裡，由重算 warning 的地方
// 一併附上。空字串代表目前沒有空間問題。
// 受 file_info_mutex 保護。
std::string g_space_note;

// 上一輪是不是因為「磁碟空間不足」而收手。
//
// 空間不足跟「跑完卻沒改善」是兩件事:前者是環境暫時不夠用,清出空間後就會成功,
// 所以不能計入收斂保護的無進展次數——計進去的話兩輪就閂鎖,之後即使空間清出來了
// 也永遠不會再試,而警告文字明明寫著「會自動繼續」(實測就是卡在這)。
std::atomic<bool> last_copy_blocked_by_space{false};
std::atomic<bool> copy_running{false};
std::thread copy_thread;

// ======================
// emmcdl.exe lookup (case-insensitive)
// ======================
// Windows 檔名不分大小寫，但實際的 installer 包裡這個檔案可能叫
// EMMCDL.exe / emmcdl.exe / EmmcDL.exe。std::filesystem::exists() 拿固定
// 字串去比對在 MinGW 上不保證能匹配到不同大小寫的檔名，一旦漏判，整條
// 流程都會認為 installer 還沒就緒而無限重複複製。
// 回傳實際找到的檔案路徑，找不到則回傳空 path。
std::filesystem::path find_emmcdl(const std::filesystem::path& dir)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec))
        return {};

    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;

        std::string name = e.path().filename().string();
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        if (lower == "emmcdl.exe")
            return e.path();
    }
    return {};
}

bool has_emmcdl(const std::filesystem::path& dir)
{
    return !find_emmcdl(dir).empty();
}

// ======================
// installer 完整性檢查：rawprogram0.xml / WDFlash.xml 所需檔案
// ======================
// 遞迴列出 dir 底下所有檔案，key 為小寫檔名，用來做一次性的大小寫不敏感
// 查找（emmcdl.exe / rawprogram0.xml / WDFlash.xml / XML 內列出的每個 .bin
// 都靠這份 index 查，不必各自重新掃一次）。
std::unordered_map<std::string, std::filesystem::path> index_files_recursive(const std::filesystem::path& dir)
{
    std::unordered_map<std::string, std::filesystem::path> out;
    std::error_code ec;

    std::filesystem::recursive_directory_iterator it(
        dir, std::filesystem::directory_options::skip_permission_denied, ec);
    std::filesystem::recursive_directory_iterator end;

    for (; !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec)) continue;

        std::string name = it->path().filename().string();
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        // 同名檔案出現在多個 subfolder 時保留第一個找到的，足夠用來判斷「有沒有」。
        out.emplace(std::move(name), it->path());
    }
    return out;
}

// 從 rawprogram0.xml / WDFlash.xml 掃出所有 <program filename="xxx.bin" .../>
// 的檔名。這兩個檔案是扁平的 Qualcomm EDL XML，用簡單字串掃描找
// filename="..." / filename='...' 屬性即可，不需要真的解析 XML 結構。
// filename=""（erase-only / GPT backup slot 常見）代表這個 program 不需要
// 檔案，直接跳過。回傳值大小寫不敏感去重，保留第一次出現的原始大小寫。
std::vector<std::string> extract_required_filenames_from_xml(const std::filesystem::path& xml_path)
{
    std::vector<std::string> result;

    std::ifstream f(xml_path, std::ios::binary);
    if (!f) return result;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    std::unordered_set<std::string> seen;
    static const std::string keys[] = {"filename=\"", "filename='"};

    for (const auto& key : keys) {
        size_t pos = 0;
        while ((pos = content.find(key, pos)) != std::string::npos) {
            size_t start = pos + key.size();
            char quote = key.back();
            size_t end = content.find(quote, start);
            if (end == std::string::npos) break;

            std::string fname = content.substr(start, end - start);
            pos = end + 1;

            if (fname.empty()) continue;

            std::string lower = fname;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c){ return std::tolower(c); });
            if (seen.insert(lower).second)
                result.push_back(fname);
        }
    }
    return result;
}

struct InstallerValidation {
    bool ok = true;
    std::vector<std::string> missing;  // 缺少的檔名（原始大小寫，供顯示用）

    // 非致命：來源（資料夾／zip）本身就沒有提供的必要 XML。
    // 目的地缺、但來源有 → 複製就能補，算 missing；兩邊都沒有 → 再抓幾次也不會
    // 出現，列在這裡當警告，不讓 installer 卡在 FAILED（見 validate 內的說明）。
    std::vector<std::string> warnings;

    // 「骨架」是否齊備：emmcdl.exe 以及這個 flash_stage 需要的 XML 本身。
    // 骨架不齊代表目的地還沒有一份可辨識的 installer（全新／空目錄／複製到
    // 一半就中斷），這種狀態下只補 XML 列出的必要檔案會留下一個殘缺的資料夾，
    // 所以呼叫端要用整包複製而不是 delta。見 use_delta 的判斷。
    bool skeleton_ok = false;
};

// 檢查 installer 資料夾裡（遞迴，含所有 subfolder）是否具備燒錄所需的所有
// 檔案：emmcdl.exe、依 flash_stage 決定需要的 rawprogram0.xml/WDFlash.xml
// 本身，以及這些 XML 內列出的每一個檔案。
//
// src_dir 可選：給了的話，除了看目的地檔案存不存在，還會反查來源（資料夾
// 或 zip）比對檔案大小——目的地檔案存在但大小跟來源不一致，代表複製/解壓
// 中斷或殘留舊版，一樣視為缺檔（推進 result.missing），才會被後續的 delta
// copy 用覆蓋語意的複製/解壓補正確。不給 src_dir 時維持舊行為（只看存不
// 存在），呼叫端還沒準備好傳來源路徑時可以直接沿用。
InstallerValidation validate_installer_files(const std::filesystem::path& dir,
                                              FlashStage flash_stage,
                                              const std::filesystem::path& src_dir = {})
{
    InstallerValidation result;

    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        result.ok = false;
        result.missing.push_back("emmcdl.exe");
        return result;
    }

    auto index = index_files_recursive(dir);

    auto to_lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        return s;
    };

    bool src_is_dir = false, src_is_zip = false;
    std::unordered_map<std::string, std::filesystem::path> src_index;
    std::unordered_map<std::string, uint64_t> zip_sizes;

    if (!src_dir.empty()) {
        std::error_code sec;
        if (std::filesystem::is_directory(src_dir, sec)) {
            src_is_dir = true;
            src_index = index_files_recursive(src_dir);
        } else if (is_zip_file(src_dir.string())) {
            src_is_zip = true;
            zip_sizes = get_zip_entry_sizes(src_dir.string());
        }
    }

    // 解析一個「相對 installer 根目錄」的名字到實際檔案。
    //
    // 一律先試 dir / name，找不到才退回 basename index。順序很重要：manifest 裡
    // 的 filename="gpt_main0.bin" 指的是跟 XML 同層（根目錄）那一份，但同名檔案
    // 在多個 NOR 變體子資料夾各有一份且大小不同
    // （nor_oob\gpt_main0.bin、dualnor_32_16mb\nor0\gpt_main0.bin ...）。
    // index_files_recursive 是 first-found-wins 的 basename 表，深度優先走訪會
    // 隨機挑到某個變體；來源端查表卻拿到根目錄那一份，兩邊大小不一致，於是
    // 一個「其實完全正確」的目的地被判成缺檔，複製完又立刻再判缺檔，正是
    // 重跑不收斂的來源。先走 dir / name 就同時涵蓋純檔名跟帶子資料夾的相對路徑。
    auto resolve_in = [&](const std::filesystem::path& base,
                          const std::unordered_map<std::string, std::filesystem::path>& idx,
                          const std::string& name) -> std::filesystem::path {
        std::error_code fec;
        std::filesystem::path direct = base / name;
        if (std::filesystem::exists(direct, fec) &&
            !std::filesystem::is_directory(direct, fec)) return direct;

        auto found = idx.find(to_lower(name));
        if (found != idx.end()) return found->second;
        return {};
    };

    // 來源大小查詢：資料夾走 resolve_in，zip 先查「正規化後的相對路徑」這個
    // 精確 key，查不到才退回純檔名。
    auto src_size_of = [&](const std::string& name, uint64_t& out) -> bool {
        if (src_is_dir) {
            std::filesystem::path f = resolve_in(src_dir, src_index, name);
            if (f.empty()) return false;
            std::error_code sec;
            out = std::filesystem::file_size(f, sec);
            return !sec;
        }
        if (src_is_zip) {
            std::string key = to_lower(name);
            std::replace(key.begin(), key.end(), '/', '\\');
            auto z = zip_sizes.find(key);
            if (z != zip_sizes.end()) { out = z->second; return true; }

            size_t sl = key.rfind('\\');
            if (sl != std::string::npos) {
                auto z2 = zip_sizes.find(key.substr(sl + 1));
                if (z2 != zip_sizes.end()) { out = z2->second; return true; }
            }
        }
        return false;
    };

    auto require = [&](const std::string& name) -> bool {
        std::filesystem::path dst_file = resolve_in(dir, index, name);
        if (dst_file.empty()) {
            result.missing.push_back(name);
            return false;
        }

        // 目的地檔案存在，若有來源路徑就順便比對檔案大小：存在但大小不符代表
        // 複製／解壓中斷或殘留舊版，一樣要當缺檔補抓。
        if (!src_dir.empty()) {
            uint64_t src_size = 0;
            if (src_size_of(name, src_size)) {
                std::error_code dec;
                uint64_t dst_size = std::filesystem::file_size(dst_file, dec);
                if (!dec && dst_size != src_size) {
                    result.missing.push_back(name);
                    return false;
                }
            }
        }
        return true;
    };

    bool skeleton = require("emmcdl.exe");

    std::vector<std::string> xml_names;
    if (stage_does_spinor(flash_stage)) xml_names.push_back("rawprogram0.xml");
    if (stage_does_hlos(flash_stage))   xml_names.push_back("WDFlash.xml");

    for (const auto& xml_name : xml_names) {
        // XML 本身也要優先取根目錄那一份。子資料夾的變體 manifest 內容不同
        // （會引用 nor_oob\... 這種帶前綴的路徑），拿錯的那份去驗會要求一組
        // 根本不該要求的檔案。
        std::filesystem::path xml_path = resolve_in(dir, index, xml_name);
        if (xml_path.empty()) {
            // 目的地沒有這份 XML，先問「來源有沒有」再決定嚴重程度：
            //   來源有   → 複製／delta 就能補上，照舊算缺檔並觸發複製。
            //   來源沒有 → 重抓幾次都不會出現。以前這裡一律算缺檔，收斂保護
            //              連續兩輪沒進展後就把整個 installer 打成
            //              COPY_FAILED，操作員只看到 FAILED 卻沒得救。
            //              改成降級為警告：installer 仍視為就緒，但把檔名
            //              帶到 UI 上。
            // 來源類型無法判定時（沒給 src_dir，或路徑既不是資料夾也不是 zip）
            // 沒有依據說「來源沒有」，維持原本的缺檔行為，不要誤放行。
            uint64_t src_size_ignored = 0;
            bool src_known = src_is_dir || src_is_zip;
            if (src_known && !src_size_of(xml_name, src_size_ignored)) {
                result.warnings.push_back(xml_name);
            } else {
                result.missing.push_back(xml_name);
                skeleton = false;
            }
            continue;
        }

        for (const auto& needed : extract_required_filenames_from_xml(xml_path))
            require(needed);
    }

    result.skeleton_ok = skeleton;
    result.ok = result.missing.empty();
    return result;
}

// 把 validation.warnings 整理成一行給操作員看的警告。
// 刻意把後果講清楚：installer 被當成就緒，但少了 XML 的那個 stage 交給 emmcdl
// 一定會失敗，看到這行的人要自己判斷是不是選錯 stage 或指錯 download path。
std::string format_installer_warning(const std::vector<std::string>& warnings)
{
    if (warnings.empty()) return "";

    std::string joined;
    for (size_t i = 0; i < warnings.size(); ++i) {
        if (i) joined += ", ";
        joined += warnings[i];
    }
    return "Source does not provide: " + joined +
           " - installer is treated as ready, but flashing the stage that needs "
           "this file will fail in emmcdl. Check the flash stage / download path "
           "if this is unexpected.";
}

// 複製或解壓正在進行中。兩種來源用不同狀態顯示（COPYING / UNZIPPING），但所有
// 「正在忙，別動它」的判斷都要同時涵蓋兩者，漏一個就會在解壓途中被當成閒置。
inline bool is_copy_in_progress(FileStatus s)
{
    return s == FileStatus::COPYING || s == FileStatus::UNZIPPING;
}

// 把「缺檔」與「來源根本沒提供」兩類異常合成一行提示。
//
// 成功／失敗只看複製或解壓這件事有沒有做完；檔案不齊不再讓 installer 變成
// FAILED，而是以 success + 這行 warning 呈現，操作員才知道少了什麼。
std::string format_installer_notes(const std::vector<std::string>& missing,
                                   const std::vector<std::string>& warnings)
{
    auto join = [](const std::vector<std::string>& v) {
        std::string s;
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) s += ", ";
            s += v[i];
        }
        return s;
    };

    std::string out;
    if (!missing.empty()) {
        out = "Missing " + std::to_string(missing.size()) + " required file(s): " +
              join(missing) + ".";
    }

    std::string warn = format_installer_warning(warnings);
    if (!warn.empty()) {
        if (!out.empty()) out += " ";
        out += warn;
    }
    return out;
}

// Helper function to send file status
void send_file_status(Talker& talker, const FileInfo& info)
{
    // Serialize FileInfo
    std::vector<char> buffer;
    
    // Status (4 bytes)
    int status_int = static_cast<int>(info.status);
    buffer.insert(buffer.end(), 
        reinterpret_cast<const char*>(&status_int), 
        reinterpret_cast<const char*>(&status_int) + sizeof(int));
    
    // Total bytes (8 bytes)
    buffer.insert(buffer.end(), 
        reinterpret_cast<const char*>(&info.total_bytes), 
        reinterpret_cast<const char*>(&info.total_bytes) + sizeof(uint64_t));
    
    // Copied bytes (8 bytes)
    buffer.insert(buffer.end(), 
        reinterpret_cast<const char*>(&info.copied_bytes), 
        reinterpret_cast<const char*>(&info.copied_bytes) + sizeof(uint64_t));
    
    // Progress (8 bytes)
    buffer.insert(buffer.end(), 
        reinterpret_cast<const char*>(&info.progress), 
        reinterpret_cast<const char*>(&info.progress) + sizeof(double));
    
    // Speed (8 bytes)
    buffer.insert(buffer.end(), 
        reinterpret_cast<const char*>(&info.speed_mbps), 
        reinterpret_cast<const char*>(&info.speed_mbps) + sizeof(double));
    
    // ETA (4 bytes)
    buffer.insert(buffer.end(), 
        reinterpret_cast<const char*>(&info.eta_seconds), 
        reinterpret_cast<const char*>(&info.eta_seconds) + sizeof(int));
    
    // Error message length (4 bytes)
    uint32_t error_len = static_cast<uint32_t>(info.error_msg.size());
    buffer.insert(buffer.end(), 
        reinterpret_cast<const char*>(&error_len), 
        reinterpret_cast<const char*>(&error_len) + sizeof(uint32_t));
    
    // Error message
    buffer.insert(buffer.end(), info.error_msg.begin(), info.error_msg.end());

    // Warning message length + payload（附加在最尾端）
    // 舊版 server 讀完 error_msg 就停止解析，多出來的這段會被忽略，
    // 所以新 client 對舊 server 依然相容。
    uint32_t warning_len = static_cast<uint32_t>(info.warning_msg.size());
    buffer.insert(buffer.end(),
        reinterpret_cast<const char*>(&warning_len),
        reinterpret_cast<const char*>(&warning_len) + sizeof(uint32_t));
    buffer.insert(buffer.end(), info.warning_msg.begin(), info.warning_msg.end());

    // File counters（再附加在最尾端，同樣是舊 server 相容的擴充）
    buffer.insert(buffer.end(),
        reinterpret_cast<const char*>(&info.current_file),
        reinterpret_cast<const char*>(&info.current_file) + sizeof(int));
    buffer.insert(buffer.end(),
        reinterpret_cast<const char*>(&info.total_files),
        reinterpret_cast<const char*>(&info.total_files) + sizeof(int));

    talker.send_msg(MSG_FILE_STATUS, buffer.data(), buffer.size());
}


#ifdef _WIN32
// 每次進度回呼都更新，watchdog 執行緒用來判斷這個檔案是否卡住。
struct CopyProgressState {
    std::atomic<uint64_t> last_bytes{0};
    std::atomic<uint64_t> total_bytes{0};
    std::atomic<long long> last_update_ms{0};
    std::atomic<bool>* cancel = nullptr;
};

static long long steady_now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

DWORD CALLBACK CopyProgressRoutine(
    LARGE_INTEGER TotalFileSize,
    LARGE_INTEGER TotalBytesTransferred,
    LARGE_INTEGER StreamSize,
    LARGE_INTEGER StreamBytesTransferred,
    DWORD dwStreamNumber,
    DWORD dwCallbackReason,
    HANDLE hSourceFile,
    HANDLE hDestinationFile,
    LPVOID lpData)
{
    CopyProgressState* state =
        static_cast<CopyProgressState*>(lpData);

    if (state) {
        state->last_bytes.store(static_cast<uint64_t>(TotalBytesTransferred.QuadPart));
        state->total_bytes.store(static_cast<uint64_t>(TotalFileSize.QuadPart));
        state->last_update_ms.store(steady_now_ms());
    }

    if (state && state->cancel && state->cancel->load())
        return PROGRESS_CANCEL;

    return PROGRESS_CONTINUE;
}

// 單檔複製若卡在單次 ReadFile/WriteFile 內部（常見原因：防毒軟體即時掃描剛
// 寫入的 .exe），CopyProgressRoutine 完全不會被呼叫，一般的心跳/進度回報因此
// 看不到任何動靜——外層迴圈永遠等不到下一次 iteration。這裡把 CopyFileExW
// 丟到獨立執行緒跑，本執行緒輪詢最後一次進度時間；超過門檻就用
// CancelSynchronousIo 打斷卡住的 thread，讓它回傳錯誤而不是永遠卡死。
//
// ⚠️ TotalBytesTransferred 回報的是「已排入寫入」的量，不是「已真正落盤」的量：
// 對大檔案，Windows 常常一口氣把 byte count 報到等於 TotalFileSize（回呼瞬間
// 跳到 100%），之後還要花時間把 cache 實際 flush 到磁碟（尤其是網路磁碟機/
// USB）才會讓 CopyFileExW 真正返回——這段時間 last_bytes 不會再變，但複製
// 並沒有卡住，只是在等 I/O flush。所以「已回報滿額」和「真正卡在傳輸中」要
// 分開處理：滿額後只印警告觀察，不能觸發強制中止，否則會把正常的大檔案
// flush 誤判成 stall，白白中止一個其實快完成的複製、還得整個重來。
static constexpr int COPY_STALL_WARN_SEC    = 5;
static constexpr int COPY_STALL_TIMEOUT_SEC = 60;
static constexpr int COPY_FLUSH_TIMEOUT_SEC = 15 * 60;

bool copy_file_win32(const std::wstring& src,
                     const std::wstring& dst,
                     std::atomic<bool>* cancel,
                     const std::function<void(uint64_t, uint64_t)>& on_progress = nullptr,
                     DWORD* out_err = nullptr)
{
    CopyProgressState state;
    state.cancel = cancel;
    state.last_update_ms.store(steady_now_ms());

    BOOL ok = FALSE;
    DWORD err = 0;
    std::atomic<bool> done{false};

    std::thread copy_op([&]() {
        ok = CopyFileExW(
            src.c_str(),
            dst.c_str(),
            CopyProgressRoutine,
            &state,
            nullptr,
            COPY_FILE_RESTARTABLE
        );
        if (!ok)
            err = GetLastError();
        done.store(true);
    });

    HANDLE thread_handle = reinterpret_cast<HANDLE>(copy_op.native_handle());
    bool cancelled_for_stall = false;
    int  triggered_timeout_sec = 0;

    while (!done.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        if (done.load())
            break;

        long long now_ms   = steady_now_ms();
        long long idle_ms  = now_ms - state.last_update_ms.load();

        uint64_t total  = state.total_bytes.load();
        uint64_t copied = state.last_bytes.load();

        // 🔧 定期把「目前這個檔案」的即時傳輸量往外回報，讓 copy_worker
        // 可以在單一大檔案複製中途持續更新 global_file_info，不用等這
        // 個檔案整個複製完成才動一次進度。
        if (on_progress)
            on_progress(copied, total);

        // 已回報滿額（copied >= total > 0）代表傳輸已完成，剩下的是等待
        // flush 落盤，不是卡在傳輸中，因此套用更長的門檻。
        bool fully_reported = (total > 0 && copied >= total);
        int  timeout_sec    = fully_reported ? COPY_FLUSH_TIMEOUT_SEC : COPY_STALL_TIMEOUT_SEC;

        if (idle_ms >= COPY_STALL_WARN_SEC * 1000)
        {
            std::cout << ts() << "[COPY] stall watch: "
                      << (copied / 1024.0 / 1024.0) << " / "
                      << (total / 1024.0 / 1024.0) << " MB"
                      << (fully_reported ? " (fully reported, waiting for disk flush)" : "")
                      << ", no progress for " << (idle_ms / 1000) << "s\n";
        }

        if (idle_ms >= static_cast<long long>(timeout_sec) * 1000 && !cancelled_for_stall)
        {
            std::cout << ts() << "[COPY WARNING] No progress for "
                      << timeout_sec
                      << "s, aborting stalled copy of this file...\n";
            CancelSynchronousIo(thread_handle);
            cancelled_for_stall = true;
            triggered_timeout_sec = timeout_sec;
            // 重置視窗，避免還沒真正中止前每一輪都再打一次 cancel
            state.last_update_ms.store(now_ms);
        }
    }

    copy_op.join();

    if (!ok)
    {
        if (err == ERROR_REQUEST_ABORTED)
            std::cout << ts() << "[COPY] cancelled\n";
        else if (cancelled_for_stall)
            std::cout << ts() << "[COPY ERROR] Copy stalled with no progress for "
                      << triggered_timeout_sec << "s and was aborted (err=" << err << ")\n";
        else
            std::cout << ts() << "[COPY ERROR] CopyFileEx failed: " << err << "\n";

        if (out_err) *out_err = err;
        return false;
    }

    if (out_err) *out_err = 0;
    return true;
}
#endif

// 目的地所在磁碟的可用空間。取不到（路徑還不存在、查詢失敗）時回傳 false，
// 呼叫端就當作「無從判斷」而不是「不足」，以免把好的複製攔下來。
bool destination_free_bytes(const std::filesystem::path& dst, uint64_t& out_free)
{
    std::error_code ec;
    std::filesystem::path probe = dst;

    // dst 可能還沒建立，往上找第一個存在的祖先來問。
    while (!probe.empty() && !std::filesystem::exists(probe, ec)) {
        auto parent = probe.parent_path();
        if (parent == probe) break;
        probe = parent;
    }
    if (probe.empty()) return false;

#ifdef _WIN32
    // std::filesystem::space() 在這個 MinGW 上會回 available = (uintmax_t)-1 而且
    // 不設 error_code——「查不到」卻裝作成功，讓空間檢查靜靜失效（實測踩到）。
    // 用 Win32 原生 API，UNC 路徑也支援。
    ULARGE_INTEGER avail{}, total_b{}, freeb{};
    std::wstring wprobe = probe.wstring();
    if (!wprobe.empty() && wprobe.back() != L'\\') wprobe += L'\\';
    if (!GetDiskFreeSpaceExW(wprobe.c_str(), &avail, &total_b, &freeb)) return false;
    out_free = static_cast<uint64_t>(avail.QuadPart);
    return true;
#else
    auto info = std::filesystem::space(probe, ec);
    if (ec || info.available == static_cast<uintmax_t>(-1)) return false;

    out_free = static_cast<uint64_t>(info.available);
    return true;
#endif
}

// 空間不足的說明文字。刻意把三個數字都寫出來，操作員才知道要清出多少。
std::string format_space_note(uint64_t need, uint64_t have)
{
    auto gb = [](uint64_t b) {
        std::ostringstream o;
        o << std::fixed << std::setprecision(2) << (b / 1024.0 / 1024.0 / 1024.0);
        return o.str();
    };
    uint64_t short_by = need > have ? need - have : 0;
    return "Not enough disk space: needs " + gb(need) + " GB, only " + gb(have) +
           " GB free (short by " + gb(short_by) + " GB). Copy was not attempted; "
           "free up space and it will continue automatically.";
}

void copy_worker(std::string src, std::string dst, std::string ip,
                  std::vector<std::string> only_names = {})
{
    // 建立長連線 Talker，整個 copy_worker 生命週期共用，避免頻繁建立/關閉 socket 干擾 heartbeat
    Talker copy_talker(ip, 9000);

    // 來源是 ZIP 還是資料夾決定顯示的狀態（UNZIPPING / COPYING）。用副檔名判斷，
    // 跟下面實際分流的條件同一套，避免 UI 先顯示 COPYING 再跳成 UNZIPPING。
    bool src_is_zip_for_status;
    {
        std::string lower = src;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        src_is_zip_for_status = lower.size() >= 4 && lower.rfind(".zip") == lower.size() - 4;
    }

    // Initialize file status
    FileInfo initial_info;
    {
        std::lock_guard<std::mutex> lock(file_info_mutex);
        global_file_info.status = src_is_zip_for_status ? FileStatus::UNZIPPING
                                                       : FileStatus::COPYING;
        global_file_info.progress = 0.0;
        global_file_info.copied_bytes = 0;
        global_file_info.total_bytes = 0;
        global_file_info.speed_mbps = 0.0;
        global_file_info.eta_seconds = 0;
        global_file_info.error_msg = "";
        // 新的一輪開始，清掉前一輪留下的過期提示——但「目前仍然成立的空間不足」
        // 要留著。
        //
        // 無條件清會閃爍：觸發是 2 秒一次，每輪開頭清掉、空間檢查後又設回去，
        // UI 就會看到警告一亮一暗（實測如此）。所以只有在沒有進行中的空間問題時
        // 才清 warning_msg；空間問題本身則由「換路徑」或「檢查通過」來清除。
        if (g_space_note.empty())
            global_file_info.warning_msg = "";
        else
            global_file_info.warning_msg = g_space_note;
        initial_info = global_file_info;
    }
    send_file_status(copy_talker, initial_info);

    // 這一輪開始，先清掉「上一次被取消」的記號
    last_copy_cancelled = false;

    // 錯誤路徑共用：鎖內設定狀態並複製，鎖外發送
    auto fail_with = [&](const std::string& msg) {
        // 被取消不算「跑完但沒改善」，不能讓收斂保護把它計為一次無進展。
        if (copy_cancel) last_copy_cancelled = true;
        FileInfo info_to_send;
        {
            std::lock_guard<std::mutex> lock(file_info_mutex);
            global_file_info.status = FileStatus::COPY_FAILED;
            global_file_info.error_msg = msg;
            info_to_send = global_file_info;
        }
        send_file_status(copy_talker, info_to_send);
        std::cout << ts() << "[COPY ERROR] " << msg << std::endl;
    };


    if (src.empty())
    {
        fail_with("Source path is empty");
        return;
    }
    if (dst.empty()){
        fail_with("Destination path is empty");
        return;
    }


    std::filesystem::path src_path(src);
    std::filesystem::path dst_path(dst);

    if (!std::filesystem::exists(src_path))
    {
        fail_with("Source path not exist: " + src);
        return;
    }




















































































            // 用副檔名判斷是否為 ZIP（避免 UNC 路徑的 std::filesystem 相容問題）
    auto src_lower = src;
    std::transform(src_lower.begin(), src_lower.end(), src_lower.begin(), ::tolower);
    bool src_is_zip = src_lower.size() >= 4 && src_lower.rfind(".zip") == src_lower.size() - 4;

    if (src_is_zip)
    {
        std::cout << ts() << "[COPY] Detected ZIP file, will extract instead of copy" << std::endl;

        // 直接解壓縮到 installer_path（emmcdl.exe 應在此目錄下）
        std::filesystem::path extract_path = dst_path;
        std::filesystem::create_directories(extract_path);

        std::cout << ts() << "[COPY] Extract destination: " << extract_path.string() << std::endl;

        if (!only_names.empty()) {
            std::cout << ts() << "[COPY] Delta copy: " << only_names.size() << " missing file(s)" << std::endl;
        }

        // ZIP 解壓的總量要等 unzip_file 掃完 zip 內容才知道（壓縮檔大小跟解壓
        // 後的總量可能差幾百倍，拿來當分母會算出完全錯誤的百分比）。先留 0，
        // unzip_file 解析到 TOTAL_BYTES 就會立刻回呼把正確總量補上。
        {
            std::lock_guard<std::mutex> lock(file_info_mutex);
            global_file_info.status = FileStatus::UNZIPPING;
            global_file_info.total_bytes = 0;
            global_file_info.copied_bytes = 0;
            global_file_info.progress = 0.0;
        }
        send_file_status(copy_talker, global_file_info);
        
        auto start_time = std::chrono::steady_clock::now();

        // Progress callback to update file_info
        // ⚠️ 這個 callback 是被 unzip_file 的 pipe 讀取迴圈同步呼叫的：
        //    每次 PowerShell 吐一行 PROGRESS:，這裡就會被呼叫一次。
        //    絕對不能在這裡做網路 send()——server 若處理不及造成 TCP
        //    背壓，send() 會阻塞好幾秒，連帶讓 pipe 讀取迴圈卡住，
        //    PowerShell 那端的 pipe buffer 寫滿後也會真的卡死，
        //    複製進度因此完全停滯（不只是 UI 沒更新）。
        //    進度回報改交給 com_monitor 既有的 2 秒定期廣播，這裡只更新
        //    記憶體狀態。
        auto progress_cb = [&](int current_file, int total_files, uint64_t current_bytes, uint64_t total_bytes) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();

            std::lock_guard<std::mutex> lock(file_info_mutex);
            global_file_info.copied_bytes = current_bytes;
            global_file_info.total_bytes = total_bytes;
            global_file_info.progress = total_bytes > 0 ? (double)current_bytes / total_bytes * 100.0 : 0.0;
            // unzip_file 已經在算「第幾個 / 共幾個 entry」，直接轉給 UI。
            global_file_info.current_file = current_file;
            global_file_info.total_files = total_files;

            if (elapsed > 0) {
                double mb_copied = current_bytes / (1024.0 * 1024.0);
                global_file_info.speed_mbps = mb_copied / elapsed;

                uint64_t remaining = total_bytes - current_bytes;
                if (current_bytes > 0) {
                    global_file_info.eta_seconds = (int)(remaining * elapsed / current_bytes);
                }
            }
        };

        // 解壓時「保留 zip 內的目錄結構」（flatten = false）。
        //
        // 這裡絕對不能用 flatten：實際的 installer zip 裡同一個檔名會在多個
        // NOR 變體子資料夾各存一份（nor_oob\rawprogram0.xml、
        // dualnor_32_16mb\nor0\rawprogram0.xml、根目錄...）。flatten 只用檔名
        // 當輸出路徑，結果是子資料夾的變體會覆蓋掉根目錄真正的 manifest，
        // 目的地變成「檔案都在但 rawprogram0.xml 是錯變體」的壞 installer；
        // 而那份變體 manifest 又會要求 nor_oob\xxx.bin 這種帶子資料夾的路徑，
        // flatten 永遠不會建子資料夾，於是驗證永遠缺檔、永遠重抓整包，
        // 形成沒有終點的重解迴圈。保留結構同時修掉「壞 installer」跟「無限重跑」。
        std::string unzip_error;
        bool zip_no_space = false;
        bool success = unzip_file(src, extract_path.string(), false, progress_cb, &unzip_error,
                                 only_names, &copy_cancel, &zip_no_space);

        if (success)
        {
            std::cout << ts() << "[COPY] ZIP extraction completed successfully" << std::endl;

            // Delta 解壓不保證來源裡真的有這個檔案（unzip_file 找不到就跳過）。
            // 解壓完後逐一確認目的地是否真的有了。
            //
            // 找不到的檔案不算「這次解壓失敗」——解壓這件事本身確實跑完了，
            // 只是來源沒有那幾個檔案。所以走 success + 一行說明，而不是
            // COPY_FAILED；否則一個「來源本來就缺一個檔」的 installer 會永遠
            // 顯示 FAILED，即使它其他部分完全可用。
            std::string unresolved_note;
            if (!only_names.empty()) {
                std::vector<std::string> unresolved;
                for (const auto& name : only_names) {
                    std::error_code fec;
                    if (!std::filesystem::exists(extract_path / name, fec))
                        unresolved.push_back(name);
                }
                if (!unresolved.empty()) {
                    std::string joined;
                    for (size_t i = 0; i < unresolved.size(); ++i) {
                        if (i) joined += ", ";
                        joined += unresolved[i];
                    }
                    unresolved_note = "Not present in source: " + joined;
                    std::cout << ts() << "[COPY] " << unresolved_note << std::endl;
                }
            }

            // Calculate total size of extracted files
            uint64_t total_size = 0;
            try {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(extract_path)) {
                    if (entry.is_regular_file()) {
                        total_size += entry.file_size();
                    }
                }
            } catch (...) {}

            FileInfo info_to_send;
            {
                std::lock_guard<std::mutex> lock(file_info_mutex);
                global_file_info.status = FileStatus::COPY_COMPLETE;
                global_file_info.progress = 100.0;
                global_file_info.total_bytes = total_size;
                global_file_info.copied_bytes = total_size;
                global_file_info.error_msg = "";
                if (!unresolved_note.empty())
                    global_file_info.warning_msg = unresolved_note;
                // 解壓成功代表空間是夠的，清掉之前留下的空間警告。
                g_space_note.clear();
                info_to_send = global_file_info;
            }
            send_file_status(copy_talker, info_to_send);
        }
        else if (zip_no_space)
        {
            // 空間不足:解壓在寫任何 entry 之前就收手了,依要求只給 warning。
            // 訊息直接用 unzip_file 產生的那份——那裡才拿得到扣掉續傳跳過後的
            // 精確需求量,在這裡重算會得到 0（total_bytes 還沒被更新)。
            std::string note = unzip_error.empty()
                ? std::string("Not enough disk space for extraction")
                : unzip_error;
            last_copy_blocked_by_space = true;

            FileInfo info_to_send;
            bool note_changed;
            {
                std::lock_guard<std::mutex> lock(file_info_mutex);
                note_changed = (g_space_note != note);
                g_space_note = note;
                global_file_info.warning_msg = note;
                global_file_info.error_msg = "";
                global_file_info.status = FileStatus::COPY_COMPLETE;
                global_file_info.progress = 0.0;
                global_file_info.copied_bytes = 0;
                info_to_send = global_file_info;
            }
            if (note_changed)
                std::cout << ts() << "[COPY WARNING] " << note << std::endl;
            send_file_status(copy_talker, info_to_send);
        }
        else
        {
            std::cout << ts() << "[COPY ERROR] ZIP extraction failed" << std::endl;

            // 空間不是這次失敗的原因，把舊的空間警告清掉，不要讓它掛在別的錯誤上。
            {
                std::lock_guard<std::mutex> lock(file_info_mutex);
                g_space_note.clear();
            }

            // 🔧 解壓失敗不再刪掉目的地資料夾。
            // installer_path 是使用者指定的路徑，可能本來就有內容（前一次成功的
            // installer、或別的東西）。以前這裡無條件 remove_all，一次誤判失敗
            // （例如 pipe 還沒抽乾就判定沒收到 COMPLETE）就會把幾十 GB 的正確
            // 內容整個毀掉，下一輪還得整包重抓。留著現場最多就是不完整，
            // 而「完整性」本來就由 validate_installer_files 把關
            // （has_emmcdl 不會把半套內容誤判成 ready），不需要靠刪資料夾來保證。
            std::cout << ts() << "[COPY] Keeping destination as-is; validation decides readiness\n";
            
            FileInfo info_to_send;
            {
                std::lock_guard<std::mutex> lock(file_info_mutex);
                global_file_info.status = FileStatus::COPY_FAILED;
                global_file_info.error_msg = unzip_error.empty() ? "ZIP extraction failed" : unzip_error;
                info_to_send = global_file_info;
            }
            send_file_status(copy_talker, info_to_send);
        }
        
        return;
    }

    // Installer Path 必須存在
    std::filesystem::create_directories(dst_path);

    try
    {
        // Delta 模式（only_names 非空）：只反查清單裡每個檔名對應的來源路徑，
        // 不用 recursive_directory_iterator 掃全部——避免缺一個幾 KB 檔案
        // 就把整個來源（現場實測 38GB/148 檔）重新複製一次。找不到來源
        // 對應檔案的記進 unresolved，等其他檔案都複製完後統一回報，不
        // 中斷整個流程。
        std::vector<std::pair<std::string, std::filesystem::path>> to_copy;  // (相對路徑字串, 來源絕對路徑)
        std::vector<std::string> unresolved;

        if (!only_names.empty() && std::filesystem::is_directory(src_path)) {
            auto src_index = index_files_recursive(src_path);
            auto to_lower_local = [](std::string s) {
                std::transform(s.begin(), s.end(), s.begin(),
                               [](unsigned char c){ return std::tolower(c); });
                return s;
            };

            for (const auto& name : only_names) {
                std::filesystem::path found_path;

                // 一律先試「相對來源根目錄」的路徑，找不到才退回 basename 索引。
                // 順序不能顛倒：同名檔案在多個 NOR 變體子資料夾各有一份且大小
                // 不同，basename 索引是 first-found-wins，深度優先會先撞到
                // dualnor_32_16mb\nor0\gpt_main0.bin（32768 bytes），把它複製到
                // 目的地的根目錄，蓋掉本來正確的 gpt_main0.bin（24576 bytes）。
                // 驗證接著又比對出大小不符 → 再補 → 又補錯，補幾次都不會好。
                std::error_code fec;
                std::filesystem::path candidate = src_path / name;
                if (std::filesystem::exists(candidate, fec) &&
                    !std::filesystem::is_directory(candidate, fec)) {
                    found_path = candidate;
                } else {
                    auto it = src_index.find(to_lower_local(name));
                    if (it != src_index.end()) found_path = it->second;
                }

                if (found_path.empty())
                    unresolved.push_back(name);
                else
                    to_copy.emplace_back(name, found_path);
            }
        }

        // 目的地已經有一份大小相同的檔案時就跳過不重抄。
        //
        // 這是「續傳」的核心：全量複製以前是無條件從第 1 個檔案重抄整份
        // installer，所以只要中途出任何狀況（按停止、磁碟滿、網路斷），
        // 已經搬好的幾十 GB 全部作廢，下一輪從 0 開始（實測 36GB 要 25 分鐘）。
        // 逐檔比對之後，重試會接續，而且「來源只更新了幾個檔」的情境也會自動變快。
        //
        // 判準用「大小相同」而不是逐位元組比對／hash：整份 installer 動輒 36GB，
        // 讀完再比會比直接重抄還慢，失去意義。這也跟
        // validate_installer_files 判斷完整性的標準一致（同樣是比大小），
        // 所以不會出現「複製說跳過、驗證卻說缺檔」互相矛盾的情況。
        auto already_copied = [](const std::filesystem::path& src_file,
                                 const std::filesystem::path& dst_file,
                                 uint64_t src_size) {
            std::error_code ec;
            if (!std::filesystem::exists(dst_file, ec)) return false;
            uint64_t dst_size = std::filesystem::file_size(dst_file, ec);
            return !ec && dst_size == src_size;
        };

        // Calculate total size（只算真正需要搬的，進度百分比才不會一開始就虛低）
        uint64_t total_size = 0;
        int total_files = 0;
        int skipped_files = 0;
        uint64_t skipped_bytes = 0;

        if (!only_names.empty() && std::filesystem::is_directory(src_path)) {
            for (const auto& item : to_copy) {
                std::error_code fec;
                total_size += std::filesystem::file_size(item.second, fec);
                total_files++;
            }
        } else if (std::filesystem::is_regular_file(src_path)) {
            total_size = std::filesystem::file_size(src_path);
            total_files = 1;
        } else if (std::filesystem::is_directory(src_path)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(src_path)) {
                if (!entry.is_regular_file()) continue;

                uint64_t sz = entry.file_size();
                std::wstring rel = entry.path().wstring().substr(src_path.wstring().size());
                if (!rel.empty() && (rel[0] == L'\\' || rel[0] == L'/')) rel.erase(0, 1);

                if (already_copied(entry.path(), dst_path / rel, sz)) {
                    skipped_files++;
                    skipped_bytes += sz;
                    continue;
                }
                total_size += sz;
                total_files++;
            }
        }

        {
            std::lock_guard<std::mutex> lock(file_info_mutex);
            global_file_info.total_bytes = total_size;
        }
        send_file_status(copy_talker, global_file_info);

        // ===== 空間預檢 =====
        // total_size 這時已經扣掉續傳跳過的檔案，所以比對的是「這一輪真的要寫多少」。
        // 空間不夠就完全不動手：硬跑下去只會在寫到大檔中途撞 ERROR_DISK_FULL，
        // 留下一個半截的檔案，而且下一輪還會再撞一次。
        //
        // 依照「空間不足只給 warning、不要 FAIL」的要求，這裡不呼叫 fail_with，
        // 而是把說明寫進 g_space_note，由 file-status 檢查連同其他 warning 一起呈現。
        {
            uint64_t free_bytes = 0;
            const uint64_t margin = 256ull * 1024 * 1024;   // 留一點餘裕，別把磁碟填到滿
            if (total_size > 0 && destination_free_bytes(dst_path, free_bytes) &&
                free_bytes < total_size + margin)
            {
                std::string note = format_space_note(total_size + margin, free_bytes);
                last_copy_blocked_by_space = true;

                FileInfo info_to_send;
                bool note_changed;
                {
                    std::lock_guard<std::mutex> lock(file_info_mutex);
                    note_changed = (g_space_note != note);
                    g_space_note = note;
                    global_file_info.warning_msg = note;
                    global_file_info.error_msg = "";
                    // 狀態不設 COPY_FAILED；目的地能不能用交由 file-status 檢查判斷。
                    global_file_info.status = FileStatus::COPY_COMPLETE;
                    global_file_info.progress = 0.0;
                    global_file_info.copied_bytes = 0;
                    info_to_send = global_file_info;
                }
                // 觸發是 2 秒一次，每輪都印會洗爆 log；只在內容變化時印。
                if (note_changed)
                    std::cout << ts() << "[COPY WARNING] " << note << std::endl;
                send_file_status(copy_talker, info_to_send);
                return;
            }
            std::lock_guard<std::mutex> lock(file_info_mutex);
            g_space_note.clear();
        }

#ifdef _WIN32
        // 單一檔案複製失敗時的統一處理。呼叫端做完就 return。
        //
        // 磁碟寫滿（112 / 39）刻意不當成 FAIL：那不是複製邏輯壞了，而是環境不夠用，
        // 依使用者要求只給 warning。清出空間後續傳會自己把剩下的接完，不需人工重跑。
        // 其他錯誤碼仍然是真正的失敗。
        auto stop_on_copy_error = [&](const std::filesystem::path& target, DWORD err) {
            if (copy_cancel) {
                fail_with("Copy cancelled");
                return;
            }
            if (err == ERROR_DISK_FULL || err == ERROR_HANDLE_DISK_FULL) {
                uint64_t free_bytes = 0;
                destination_free_bytes(dst_path, free_bytes);
                std::string note =
                    "Disk filled up while copying " + target.filename().string() + ". " +
                    format_space_note(total_size, free_bytes);
                std::cout << ts() << "[COPY WARNING] " << note << std::endl;
                last_copy_blocked_by_space = true;

                FileInfo info_to_send;
                {
                    std::lock_guard<std::mutex> lock(file_info_mutex);
                    g_space_note = note;
                    global_file_info.warning_msg = note;
                    global_file_info.error_msg = "";
                    global_file_info.status = FileStatus::COPY_COMPLETE;
                    info_to_send = global_file_info;
                }
                send_file_status(copy_talker, info_to_send);
                return;
            }
            fail_with("Failed to copy file: " + target.filename().string());
        };
#endif

        auto start_time = std::chrono::steady_clock::now();
        uint64_t copied_so_far = 0;
        int file_count = 0;

        // 🔧 統一的進度更新：不管是「單一大檔案複製中途」的即時回呼，
        // 還是「某個檔案整個複製完成」後的累計更新，都走這裡，
        // 避免 speed/eta 算法散落在多個地方各寫一份。
        auto update_progress = [&](uint64_t bytes_done) {
            std::lock_guard<std::mutex> lock(file_info_mutex);
            global_file_info.copied_bytes = bytes_done;
            global_file_info.progress = total_size > 0
                ? (double)bytes_done / total_size * 100.0
                : 0.0;
            // 檔案計數跟位元組進度一起更新，UI 才能顯示 92/148。
            global_file_info.current_file = file_count;
            global_file_info.total_files = total_files;

            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
            if (elapsed > 0 && bytes_done > 0) {
                double mb_copied = bytes_done / (1024.0 * 1024.0);
                global_file_info.speed_mbps = mb_copied / elapsed;

                uint64_t remaining = total_size - bytes_done;
                global_file_info.eta_seconds = (int)(remaining * elapsed / bytes_done);
            }
        };

        // ===== CASE 1: 單一檔案 =====
        if (std::filesystem::is_regular_file(src_path))
        {
            if (copy_cancel)
            {
                fail_with("Copy cancelled");
                return;
            }

            std::filesystem::path target =
                dst_path / src_path.filename();

#ifdef _WIN32
            if (!copy_file_win32(
                    src_path.wstring(),
                    target.wstring(),
                    &copy_cancel,
                    [&](uint64_t cur, uint64_t /*tot*/) { update_progress(cur); }))
            {
                // 取消或真正的複製錯誤都必須讓狀態離開 COPYING，
                // 否則 main loop 會永遠停在 [WAITING] Installer is copying。
                fail_with(copy_cancel ? "Copy cancelled"
                                      : "Failed to copy file: " + src_path.filename().string());
                return;
            }
#else
            // Linux / POSIX
            std::ifstream in(src, std::ios::binary);
            std::ofstream out(target, std::ios::binary);

            char buf[1024 * 64];
            while (in && out)
            {
                if (copy_cancel)
                {
                    {
                        std::lock_guard<std::mutex> lock(file_info_mutex);
                        global_file_info.status = FileStatus::COPY_FAILED;
                        global_file_info.error_msg = "Copy cancelled";
                    }
                    send_file_status(copy_talker, global_file_info);
                    std::cout << ts() << "[COPY] cancelled\n";
                    return;
                }

                in.read(buf, sizeof(buf));
                out.write(buf, in.gcount());
            }
#endif
            
            // Update progress
            copied_so_far = total_size;
            update_progress(copied_so_far);
            {
                std::lock_guard<std::mutex> lock(file_info_mutex);
                global_file_info.status = FileStatus::COPY_COMPLETE;
            }
            send_file_status(copy_talker, global_file_info);
            return;
        }

        if (std::filesystem::is_directory(src_path))
        {
            if (!only_names.empty())
            {
                // ===== Delta copy：只複製 to_copy 清單裡反查到的檔案 =====
                std::cout << ts() << "[COPY] Delta copy: " << only_names.size()
                          << " missing file(s), " << to_copy.size() << " found in source"
                          << " (exact bytes: " << total_size << ")\n";

                for (const auto& item : to_copy)
                {
                    const std::string& rel = item.first;
                    const std::filesystem::path& src_file = item.second;

                    if (copy_cancel)
                    {
                        std::cout << ts() << "[COPY] cancelled\n";
                        fail_with("Copy cancelled");
                        return;
                    }

                    std::filesystem::path target = dst_path / rel;
                    std::filesystem::create_directories(target.parent_path());

                    uint64_t file_size = 0;
                    { std::error_code fec; file_size = std::filesystem::file_size(src_file, fec); }

#ifdef _WIN32
                    DWORD cerr = 0;
                    if (!copy_file_win32(
                            src_file.wstring(),
                            target.wstring(),
                            &copy_cancel,
                            [&](uint64_t cur, uint64_t /*tot*/) { update_progress(copied_so_far + cur); },
                            &cerr))
                    {
                        stop_on_copy_error(target, cerr);
                        return;
                    }
#else
                    std::ifstream in(src_file, std::ios::binary);
                    std::ofstream out(target, std::ios::binary);

                    char buf[1024 * 64];
                    while (in && out)
                    {
                        if (copy_cancel)
                        {
                            std::cout << ts() << "[COPY] cancelled\n";
                            fail_with("Copy cancelled");
                            return;
                        }

                        in.read(buf, sizeof(buf));
                        out.write(buf, in.gcount());
                    }
#endif

                    copied_so_far += file_size;
                    file_count++;
                    update_progress(copied_so_far);
                }

                std::cout << ts() << "[COPY] Delta copy done: " << file_count << " file(s), "
                          << (copied_so_far / 1024.0 / 1024.0) << " MB\n";

                // 來源也沒有的檔案不算「這次複製失敗」——複製確實跑完了，只是
                // 來源缺那幾個檔。走 success + 一行說明，理由同 ZIP 分支。
                std::string unresolved_note;
                if (!unresolved.empty()) {
                    std::string joined;
                    for (size_t i = 0; i < unresolved.size(); ++i) {
                        if (i) joined += ", ";
                        joined += unresolved[i];
                    }
                    unresolved_note = "Not present in source: " + joined;
                    std::cout << ts() << "[COPY] " << unresolved_note << std::endl;
                }

                {
                    std::lock_guard<std::mutex> lock(file_info_mutex);
                    global_file_info.status = FileStatus::COPY_COMPLETE;
                    global_file_info.progress = 100.0;
                    global_file_info.copied_bytes = total_size;
                    global_file_info.error_msg = "";
                    if (!unresolved_note.empty())
                        global_file_info.warning_msg = unresolved_note;
                }
                send_file_status(copy_talker, global_file_info);
                return;
            }

            std::cout << ts() << "[COPY] Starting folder copy: " << total_files
                      << " files, " << (total_size / 1024.0 / 1024.0) << " MB"
                      << " (exact bytes: " << total_size << ")";
            if (skipped_files > 0) {
                std::cout << " [resuming: " << skipped_files << " file(s) / "
                          << (skipped_bytes / 1024.0 / 1024.0)
                          << " MB already present, skipped]";
            }
            std::cout << "\n";

            auto last_heartbeat_log = std::chrono::steady_clock::now();

            for (const auto& entry :
                 std::filesystem::recursive_directory_iterator(src_path))
            {
                if (copy_cancel)
                {
                    std::cout << ts() << "[COPY] cancelled\n";
                    fail_with("Copy cancelled");
                    return;
                }

                // 每 10 秒印一次進度，確認執行緒還活著、正在處理哪個檔案
                auto now_hb = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(
                        now_hb - last_heartbeat_log).count() >= 10) {
                    last_heartbeat_log = now_hb;
                    std::cout << ts() << "[COPY] alive: " << file_count << "/" << total_files
                              << " files, current=" << entry.path().filename().string() << "\n";
                }

                // 只做字串運算，避免 canonical
                std::wstring rel =
                    entry.path().wstring().substr(
                        src_path.wstring().size());

                if (!rel.empty() &&
                    (rel[0] == L'\\' || rel[0] == L'/'))
                    rel.erase(0, 1);

                std::filesystem::path target = dst_path / rel;

                if (entry.is_directory())
                {
                    std::filesystem::create_directories(target);
                }
                else if (entry.is_regular_file())
                {
                    std::filesystem::create_directories(target.parent_path());

                    uint64_t file_size = entry.file_size();

                    // 已經有一份大小相同的了 → 跳過。判準與上面算 total_size 時
                    // 用的是同一個 lambda，兩邊一致才不會讓進度分母與實際搬的量對不上。
                    if (already_copied(entry.path(), target, file_size))
                        continue;

#ifdef _WIN32
                    DWORD cerr = 0;
                    if (!copy_file_win32(
                            entry.path().wstring(),
                            target.wstring(),
                            &copy_cancel,
                            [&](uint64_t cur, uint64_t /*tot*/) { update_progress(copied_so_far + cur); },
                            &cerr))
                    {
                        // 忽略這個回傳值會讓失敗變成靜默卡死：狀態留在
                        // COPYING，main loop 永遠等不到 COPY_COMPLETE。
                        stop_on_copy_error(target, cerr);
                        return;
                    }
#else
                    std::ifstream in(entry.path(), std::ios::binary);
                    std::ofstream out(target, std::ios::binary);

                    char buf[1024 * 64];
                    while (in && out)
                    {
                        if (copy_cancel)
                        {
                            std::cout << ts() << "[COPY] cancelled\n";
                            fail_with("Copy cancelled");
                            return;
                        }

                        in.read(buf, sizeof(buf));
                        out.write(buf, in.gcount());
                    }
#endif
                    
                    // Update progress
                    copied_so_far += file_size;
                    file_count++;

                    // 只更新記憶體狀態，不在複製迴圈裡做網路 send()。
                    // 這裡若卡在 socket 背壓上，會直接拖慢複製本身（同一個
                    // 執行緒），進度回報改交給 com_monitor 的 2 秒定期廣播。
                    update_progress(copied_so_far);
                }
            }

            std::cout << ts() << "[COPY] folder done: " << file_count << " files, "
                      << (copied_so_far / 1024.0 / 1024.0) << " MB\n";

            // 診斷：複製完成後確認 emmcdl.exe 是否在目標根目錄（不分大小寫）。
            // 整條流程（com_monitor 狀態判定、needs_copy、燒錄前確認）都以
            // 「installer_path 根目錄下有 emmcdl.exe」為準，若來源把它放在
            // 子資料夾，複製會保留結構，emmcdl.exe 就不在根目錄，於是狀態
            // 會被判回 WAITING_FOR_COPY 並無限重複複製。
            if (auto found = find_emmcdl(dst_path); !found.empty()) {
                std::cout << ts() << "[COPY] emmcdl found at root: "
                          << found.filename().string() << "\n";
            } else {
                std::cout << ts() << "[COPY WARNING] emmcdl.exe NOT found at root of "
                          << dst_path.string() << "\n";
                std::error_code ec;
                for (const auto& e : std::filesystem::recursive_directory_iterator(dst_path, ec)) {
                    if (ec) break;
                    if (!e.is_regular_file(ec)) continue;
                    std::string nm = e.path().filename().string();
                    std::string low = nm;
                    std::transform(low.begin(), low.end(), low.begin(),
                                   [](unsigned char c){ return std::tolower(c); });
                    if (low == "emmcdl.exe") {
                        std::cout << ts() << "[COPY WARNING] emmcdl.exe is actually at: "
                                  << e.path().string() << "\n";
                        std::cout << ts() << "[COPY WARNING] -> installer path should point to: "
                                  << e.path().parent_path().string() << "\n";
                        break;
                    }
                }
            }
            
            // Mark as complete
            {
                std::lock_guard<std::mutex> lock(file_info_mutex);
                global_file_info.status = FileStatus::COPY_COMPLETE;
                global_file_info.progress = 100.0;
                global_file_info.copied_bytes = total_size;
            }
            send_file_status(copy_talker, global_file_info);
            return;
        }

        std::cout << ts() << "[COPY ERROR] unsupported src type\n";
        {
            std::lock_guard<std::mutex> lock(file_info_mutex);
            global_file_info.status = FileStatus::COPY_FAILED;
            global_file_info.error_msg = "Unsupported source type";
        }
        send_file_status(copy_talker, global_file_info);
    }
    catch (const std::exception& e)
    {
        std::cout << ts() << "[COPY ERROR] copy failed: "
                  << e.what() << "\n";

        {
            std::lock_guard<std::mutex> lock(file_info_mutex);
            global_file_info.status = FileStatus::COPY_FAILED;
            global_file_info.error_msg = e.what();
        }
        send_file_status(copy_talker, global_file_info);
    }
    catch (...)
    {
        // 非 std::exception 的拋出若逃出這個執行緒，狀態會永遠留在
        // COPYING，main loop 就會無限等待。一定要接住。
        std::cout << ts() << "[COPY ERROR] copy failed with unknown exception\n";

        {
            std::lock_guard<std::mutex> lock(file_info_mutex);
            global_file_info.status = FileStatus::COPY_FAILED;
            global_file_info.error_msg = "Unknown exception during copy";
        }
        send_file_status(copy_talker, global_file_info);
    }
}

std::string get_device_name()
{
    char name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = sizeof(name);

    if (GetComputerNameA(name, &size))
        return std::string(name);

    return "UNKNOWN_PC";
}

// 本機這台機器實際的 IP：跟 target_ip 建一個 UDP socket 再用 getsockname 反查，
// 借用 OS 路由表算出「連到 target_ip 會用哪張介面卡的 IP」——UDP connect()
// 不會真的送出封包，純粹是本機查表，跟遠端 host_server 用 accept() 看到的
// 來源位址是同一個答案。失敗（例如網路還沒起來）回傳空字串讓呼叫端重試。
std::string get_local_ip_facing(const std::string& target_ip)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return "";

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9);  // discard port，只用來讓路由表選介面卡，不會真的送出
    if (inet_pton(AF_INET, target_ip.c_str(), &addr.sin_addr) != 1) {
        closesocket(s);
        return "";
    }

    std::string result;
    if (connect(s, (sockaddr*)&addr, sizeof(addr)) == 0) {
        sockaddr_in local{};
        int len = sizeof(local);
        if (getsockname(s, (sockaddr*)&local, &len) == 0) {
            char buf[INET_ADDRSTRLEN] = {0};
            if (inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf)))
                result = buf;
        }
    }
    closesocket(s);
    return result;
}

// target_ip 的反解主機名。只嘗試一次（DNS 查詢可能慢，不想每次 publish 都做），
// 查不到（很多內網 IP 沒設 PTR record）就回空字串，畫面上只顯示 IP。
std::string get_server_display_name(const std::string& target_ip)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, target_ip.c_str(), &addr.sin_addr) != 1)
        return "";

    char host[NI_MAXHOST] = {0};
    if (getnameinfo((sockaddr*)&addr, sizeof(addr), host, sizeof(host), nullptr, 0, NI_NAMEREQD) == 0)
        return std::string(host);

    return "";
}

// bool is_folder_complete(const std::string& src_path, const std::string& dst_path) {
//     try {
//         if (!std::filesystem::exists(dst_path)) return false;

//         // 1. 比較檔案數量
//         auto src_iter = std::filesystem::recursive_directory_iterator(src_path);
//         auto dst_iter = std::filesystem::recursive_directory_iterator(dst_path);
        
//         size_t src_count = std::distance(std::filesystem::begin(src_iter), std::filesystem::end(src_iter));
//         size_t dst_count = std::distance(std::filesystem::begin(dst_iter), std::filesystem::end(dst_iter));

//         if (src_count != dst_count) return false;

//         // 2. 比較每個檔案的大小
//         for (const auto& entry : std::filesystem::recursive_directory_iterator(src_path)) {
//             if (std::filesystem::is_regular_file(entry)) {
//                 // 取得相對路徑
//                 auto rel_path = std::filesystem::relative(entry.path(), src_path);
//                 auto target_path = std::filesystem::path(dst_path) / rel_path;

//                 if (!std::filesystem::exists(target_path) || 
//                     std::filesystem::file_size(entry.path()) != std::filesystem::file_size(target_path)) {
//                     return false;
//                 }
//             }
//         }
//         return true;
//     } catch (...) {
//         return false;
//     }
// }

void com_monitor(const std::string& ip, Listener* listener)
{
    int copy_conuter = 0;
    std::string msg = "Heart Beat!!!";

    // 🔧 使用單一 Talker，避免頻繁創建/銷毀 socket
    Talker talker(ip, 9000);

    // heartbeat 走獨立連線、短逾時：MSG_FILE_STATUS / MSG_COMPORT 等訊息如果
    // 遇到對端一時沒在讀（例如 server 忙著處理其他 client），send() 可能卡到
    // 30 秒逾時才失敗。若 heartbeat 跟這些訊息共用同一個 socket，同一輪迴圈
    // 卡住 30 秒，heartbeat 就會遲交超過 server 端 5 秒的 DEAD 判定窗口。
    // 獨立出來後，其他訊息卡多久都不會影響存活判定。
    Talker heartbeat_talker(ip, 9000, /*timeout_sec=*/3);

    auto last_print_time = std::chrono::steady_clock::now();
    auto last_file_status_time = std::chrono::steady_clock::now();
    while (true)
    {
        auto loop_start = std::chrono::steady_clock::now();

        std::vector<int> current_coms;
        std::set<int> current_set;

                // ===== Scan current COMs =====
        auto t_scan_start = std::chrono::steady_clock::now();
        if (get_all_9008_comports(current_coms))
        {
            current_set.insert(current_coms.begin(), current_coms.end());

            // 🔧 在鎖外定義 removed_ports
            std::vector<ComportInfo> removed_ports;

            {
                std::lock_guard<std::mutex> lock(com_mutex);

                                // ===== New COM detection =====
                for (int com : current_set)
                {
                    // Check if this COM is new (not in comport_map)
                    if (comport_map.find(com) == comport_map.end())
                    {
                        // Create new ComportInfo with PENDING status
                        ComportInfo info;
                        info.number = com;
                        info.status = ComportStatus::PENDING;
                        comport_map[com] = info;

                        std::cout << ts() << "[EVENT] New COM detected: COM" << com << " [PENDING]\n";

                        // Always add to queue (regardless of auto_flash status)
                        // check if already in queue
                        bool already_in_queue = false;
                        {
                            std::queue<int> tmp = new_com_queue;  // copy to avoid locking too long
                            while (!tmp.empty()) {
                                if (tmp.front() == com) {
                                    already_in_queue = true;
                                    break;
                                }
                                tmp.pop();
                            }
                        }

                        // if not in queue, push to queue and notify
                        if (!already_in_queue)
                        {
                            new_com_queue.push(com);
                            std::cout << ts() << "[QUEUE ] Added COM" << com << " to queue";
                            if (!auto_flash) {
                                std::cout << " (waiting for auto_flash to be enabled)";
                            }
                            std::cout << "\n";
                            
                            std::cout << ts() << "[QUEUE ] new_com_queue: ";
                            std::queue<int> dump = new_com_queue;
                            if (dump.empty()) {
                                std::cout << "(empty)";
                            } else {
                                while (!dump.empty()) {
                                    std::cout << "COM" << dump.front() << " ";
                                    dump.pop();
                                }
                            }
                            std::cout << std::endl;

                            // Only notify if auto_flash is enabled
                            if (auto_flash) {
                                com_cv.notify_one();
                            }
                        }
                        else
                        {
                            std::cout << ts() << "[SKIP ] COM" << com
                                    << " already in new_com_queue\n";
                        }
                    }
                }

                                                // ===== Removed COM detection =====
                for (auto it = comport_map.begin(); it != comport_map.end(); )
                {
                    if (current_set.find(it->first) == current_set.end()) {
                        std::cout << ts() << "[EVENT] COM removed: COM" << it->first << "\n";
                        
                        // Mark as REMOVED before erasing
                        it->second.status = ComportStatus::REMOVED;
                        removed_ports.push_back(it->second);
                        
                        it = comport_map.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            
                        // 🔧 在鎖外發送 REMOVED 狀態（重用 talker，避免創建新 socket）
            for (const auto& info : removed_ports) {
                std::vector<char> buffer;
                buffer.insert(buffer.end(), 
                    reinterpret_cast<const char*>(&info.number), 
                    reinterpret_cast<const char*>(&info.number) + sizeof(int));
                
                int status_int = static_cast<int>(info.status);
                buffer.insert(buffer.end(), 
                    reinterpret_cast<const char*>(&status_int), 
                    reinterpret_cast<const char*>(&status_int) + sizeof(int));
                
                uint32_t log_length = static_cast<uint32_t>(info.log.size());
                buffer.insert(buffer.end(), 
                    reinterpret_cast<const char*>(&log_length), 
                    reinterpret_cast<const char*>(&log_length) + sizeof(uint32_t));
                
                buffer.insert(buffer.end(), info.log.begin(), info.log.end());

                // Error message (length-prefixed，加在尾端維持舊封包相容)
                uint32_t error_msg_length = static_cast<uint32_t>(info.error_msg.size());
                buffer.insert(buffer.end(),
                    reinterpret_cast<const char*>(&error_msg_length),
                    reinterpret_cast<const char*>(&error_msg_length) + sizeof(uint32_t));
                buffer.insert(buffer.end(), info.error_msg.begin(), info.error_msg.end());

                // 🔧 重用現有的 talker，不創建新的
                if (!talker.send_msg(MSG_COMPORT, buffer.data(), buffer.size())) {
                    std::cout << ts() << "[Send failed] MSG_COMPORT (REMOVED) COM" << info.number << std::endl;
                }
            }
        }

        // 🔍 診斷：SetupAPI 掃描在複製作業佔用磁碟/CPU 時可能變慢，
        // 這段耗時異常會直接拖累下面的 heartbeat 送出時機。
        {
            auto t_scan_end = std::chrono::steady_clock::now();
            auto scan_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_scan_end - t_scan_start).count();
            if (scan_ms > 500) {
                std::cout << ts() << "[PERF] get_all_9008_comports scan took " << scan_ms << "ms\n";
            }
        }


        // print logs
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(
                now - last_print_time).count() >= 60)
        {
            std::lock_guard<std::mutex> lock(com_mutex);

            // ===== print current Scan =====
            std::cout << ts() << "[SCAN ] get_all_9008_comports: ";
            if (current_set.empty()) {
                std::cout << "(none)";
            } else {
                for (int com : current_set) {
                    std::cout << "COM" << com << " ";
                }
            }
            std::cout << std::endl;

            // ===== Print current comport_map =====
            std::cout << ts() << "[STATE] comport_map:             ";
            if (comport_map.empty()) {
                std::cout << "(none)";
            } else {
                for (const auto& [com, info] : comport_map) {
                    const char* status_str = "UNKNOWN";
                    switch (info.status) {
                        case ComportStatus::PENDING:  status_str = "PENDING";  break;
                        case ComportStatus::FLASHING: status_str = "FLASHING"; break;
                        case ComportStatus::SUCCESS:  status_str = "SUCCESS";  break;
                        case ComportStatus::FAIL:     status_str = "FAIL";     break;
                    }
                    std::cout << "COM" << com << "[" << status_str << "] ";
                }
            }
            std::cout << std::endl;

            last_print_time = now;
        }


                // ===== Send heartbeat to server =====
        std::string device_name = get_device_name();

        if (!heartbeat_talker.send_msg(MSG_HEART_BEAT,
                            device_name.c_str(),
                            device_name.size()))
        {
            std::cout << ts() << "[Send failed] MSG_HEART_BEAT" << std::endl;
        }

                        // ===== Send COM ports status to server =====
        {
            // 🔧 先複製資料，減少鎖持有時間
            std::vector<std::pair<int, ComportInfo>> comports_to_send;
            {
                std::lock_guard<std::mutex> lock(com_mutex);
                for (const auto& [com, info] : comport_map)
                {
                    comports_to_send.push_back({com, info});
                }
            }
            
            // 🔧 在鎖外發送，避免阻塞 heartbeat
            for (const auto& [com, info] : comports_to_send)
            {
                // Serialize ComportInfo: int + status + log_length + log_data
                std::vector<char> buffer;
                
                // COM number (4 bytes)
                buffer.insert(buffer.end(), 
                    reinterpret_cast<const char*>(&info.number), 
                    reinterpret_cast<const char*>(&info.number) + sizeof(int));
                
                // Status (4 bytes)
                int status_int = static_cast<int>(info.status);
                buffer.insert(buffer.end(), 
                    reinterpret_cast<const char*>(&status_int), 
                    reinterpret_cast<const char*>(&status_int) + sizeof(int));
                
                // Log length (4 bytes)
                uint32_t log_length = static_cast<uint32_t>(info.log.size());
                buffer.insert(buffer.end(), 
                    reinterpret_cast<const char*>(&log_length), 
                    reinterpret_cast<const char*>(&log_length) + sizeof(uint32_t));
                
                // Log data
                buffer.insert(buffer.end(), info.log.begin(), info.log.end());

                // Error message (length-prefixed，加在尾端維持舊封包相容)
                uint32_t error_msg_length = static_cast<uint32_t>(info.error_msg.size());
                buffer.insert(buffer.end(),
                    reinterpret_cast<const char*>(&error_msg_length),
                    reinterpret_cast<const char*>(&error_msg_length) + sizeof(uint32_t));
                buffer.insert(buffer.end(), info.error_msg.begin(), info.error_msg.end());

                                if (!talker.send_msg(MSG_COMPORT, buffer.data(), buffer.size()))
                {
                    std::cout << ts() << "[Send failed] MSG_COMPORT COM" << com << std::endl;
                }
            }
        }

                                // ===== Send auto_flash status to server =====
        // ===== 套用本機 WebUI 送來的設定 =====
        // 刻意在這裡（com_monitor 自己的執行緒）套用，而不是讓 HTTP 執行緒直接寫
        // listener 的欄位：installer_path / download_path 是 std::string，
        // 本執行緒每 100ms 就在讀它們，多一個寫入者等於多一組會直接崩的 data race。
        // 套用點放在讀取這些欄位之前，改完當輪就會生效。
        {
            LocalConfigRequest req;
            if (local_server_take_config(req)) {
                if (req.installer_path) {
                    listener->installer_path = *req.installer_path;
                    std::cout << ts() << "[LOCAL] installer_path = " << *req.installer_path << std::endl;
                }
                if (req.download_path) {
                    listener->download_path = *req.download_path;
                    std::cout << ts() << "[LOCAL] download_path = " << *req.download_path << std::endl;
                }
                if (req.chipset) {
                    listener->chipset = *req.chipset;
                    std::cout << ts() << "[LOCAL] chipset = " << chipset_to_string(*req.chipset) << std::endl;
                }
                if (req.storage) {
                    listener->storage = *req.storage;
                    std::cout << ts() << "[LOCAL] storage = " << storage_to_string(*req.storage) << std::endl;
                }
                if (req.flash_stage) {
                    listener->flash_stage = *req.flash_stage;
                    std::cout << ts() << "[LOCAL] flash_stage = " << stage_to_string(*req.flash_stage) << std::endl;
                }
                // Hamoa / Glymur 只有 NVME，跟遠端 /send 一樣在這裡也夾一次。
                listener->storage = clamp_storage(listener->chipset, listener->storage);
                if (req.auto_flash) {
                    listener->set_auto_flash(*req.auto_flash);
                    std::cout << ts() << "[LOCAL] auto_flash = " << (*req.auto_flash ? "1" : "0") << std::endl;
                }
            }
        }

        bool current_auto_flash = listener->auto_flash();
        
        // Detect auto_flash state change from false to true
        if (!auto_flash && current_auto_flash) {
            std::cout << ts() << "[AUTO_FLASH] Enabled! Notifying main loop to process queue...\n";
            com_cv.notify_one();  // Wake up main loop
        }
        
        auto_flash = current_auto_flash;
        
                if (!talker.send_msg(MSG_AUTO_FLASH_STATUS,
                            &auto_flash,
                            sizeof(auto_flash)))
        {
            std::cout << ts() << "[Send failed] MSG_AUTO_FLASH_STATUS" << std::endl;
        }
        
        // ===== Send current client paths to server =====
        // 路徑為空時不送：沒有資訊量，而且舊版 server 會把長度 0 的封包
        // 當成協定錯誤而切斷連線。
        std::string current_installer = listener->installer_path;
        std::string current_download = listener->download_path;

        if (!current_installer.empty())
        {
            if (!talker.send_msg(MSG_CLIENT_INSTALLER_PATH,
                                current_installer.c_str(),
                                current_installer.size()))
            {
                std::cout << ts() << "[Send failed] MSG_CLIENT_INSTALLER_PATH" << std::endl;
            }
        }

        if (!current_download.empty())
        {
            if (!talker.send_msg(MSG_CLIENT_DOWNLOAD_PATH,
                                current_download.c_str(),
                                current_download.size()))
            {
                std::cout << ts() << "[Send failed] MSG_CLIENT_DOWNLOAD_PATH" << std::endl;
            }
        }

        // ===== Send current chipset / storage back to server =====
        Chipset current_chipset = listener->chipset;
        StorageType current_storage = clamp_storage(current_chipset, listener->storage);

        int chipset_value = static_cast<int>(current_chipset);
        if (!talker.send_msg(MSG_CLIENT_CHIPSET, &chipset_value, sizeof(chipset_value)))
        {
            std::cout << ts() << "[Send failed] MSG_CLIENT_CHIPSET" << std::endl;
        }

        int storage_value = static_cast<int>(current_storage);
        if (!talker.send_msg(MSG_CLIENT_STORAGE, &storage_value, sizeof(storage_value)))
        {
            std::cout << ts() << "[Send failed] MSG_CLIENT_STORAGE" << std::endl;
        }

        int stage_value = static_cast<int>(listener->flash_stage);
        if (!talker.send_msg(MSG_CLIENT_FLASH_STAGE, &stage_value, sizeof(stage_value)))
        {
            std::cout << ts() << "[Send failed] MSG_CLIENT_FLASH_STAGE" << std::endl;
        }
        
                                                        // ===== Check installer status and send file status =====
        // 🔧 降低發送頻率，從每 100ms 改為每 2 秒，減少 socket 創建
        auto now_file = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(
                now_file - last_file_status_time).count() >= 2)
        {
            auto t_filestatus_start = std::chrono::steady_clock::now();
            // 🔧 先讀取狀態，不在鎖內遍歷目錄
            FileStatus current_status;
            std::string installer_path_str;
            {
                std::lock_guard<std::mutex> lock(file_info_mutex);
                current_status = global_file_info.status;
            }

            // 如果正在複製/解壓縮，不要檢查檔案，保持 COPYING 狀態
            if (!is_copy_in_progress(current_status)) {
                installer_path_str = listener->installer_path;

                if (installer_path_str.empty()) {
                    // installer path 未設定，維持 NOT_FOUND
                    std::lock_guard<std::mutex> lock(file_info_mutex);
                    global_file_info.status = FileStatus::NOT_FOUND;
                    global_file_info.total_bytes = 0;
                    global_file_info.copied_bytes = 0;
                    global_file_info.progress = 0.0;
                    // 不是就緒狀態，清掉上一輪的警告，否則 UI 會顯示一個
                    // 已經不適用的提示（而且警告只在「內容改變」時才記 log，
                    // 殘留會讓下一輪真的該提示時反而不記）。
                    global_file_info.warning_msg = "";
                    global_file_info.current_file = 0;
                    global_file_info.total_files = 0;
                } else {
                    std::filesystem::path inst_path(installer_path_str);
                    bool inst_exists = std::filesystem::exists(inst_path);

                    if (inst_exists && !std::filesystem::is_directory(inst_path)) {
                        // installer path 存在但不是資料夾
                        std::lock_guard<std::mutex> lock(file_info_mutex);
                        global_file_info.status = FileStatus::PATH_NOT_A_FOLDER;
                        global_file_info.error_msg = "Installer path is not a folder: " + installer_path_str;
                        global_file_info.warning_msg = "";
                        global_file_info.current_file = 0;
                        global_file_info.total_files = 0;
                        std::cout << ts() << "[FILE_INFO] Installer path is not a folder: " << installer_path_str << std::endl;
                    } else if (inst_exists && std::filesystem::is_directory(inst_path)) {
                        // installer path 是資料夾，檢查裡面有沒有 emmcdl.exe
                        // （不分大小寫：實際包裡可能是 EMMCDL.exe）
                        bool emmcdl_found = has_emmcdl(inst_path);

                        if (emmcdl_found) {
                            // emmcdl.exe 存在，接著依 flash_stage 檢查
                            // rawprogram0.xml/WDFlash.xml 本身以及裡面列出的
                            // 每個檔案是否齊全（遞迴，含所有 subfolder）
                            InstallerValidation validation =
                                validate_installer_files(inst_path, listener->flash_stage, listener->download_path);

                            // emmcdl.exe 在 → 這個目的地就是一份可辨識、可用的
                            // installer，狀態一律回報就緒。
                            //
                            // 「檔案齊不齊」不再決定成功或失敗，只決定要不要附警告：
                            // 缺檔會由觸發邏輯去補（delta），補不到就把檔名寫在
                            // warning 上讓操作員自己判斷。以前這裡會設
                            // MISSING_FILES，於是「來源本來就少一個檔」的
                            // installer 即使其他部分完全可用也永遠顯示 FAILED。
                            std::string warn_str =
                                format_installer_notes(validation.missing, validation.warnings);
                            // 空間不足的提示要一起帶上，否則每 2 秒重算就會蓋掉
                            // copy_worker 寫的那一行，UI 永遠看不到空間問題。
                            {
                                std::lock_guard<std::mutex> lock(file_info_mutex);
                                if (!g_space_note.empty()) {
                                    if (!warn_str.empty()) warn_str += " ";
                                    warn_str += g_space_note;
                                }
                            }

                            bool recovering = (current_status == FileStatus::COPY_FAILED ||
                                               current_status == FileStatus::MISSING_FILES);

                            if (current_status != FileStatus::COPY_COMPLETE &&
                                current_status != FileStatus::FOUND) {
                                uint64_t total_size = 0;
                                try {
                                    for (const auto& entry : std::filesystem::recursive_directory_iterator(inst_path)) {
                                        if (entry.is_regular_file()) total_size += entry.file_size();
                                    }
                                } catch (...) {}
                                std::lock_guard<std::mutex> lock(file_info_mutex);
                                global_file_info.status = FileStatus::FOUND;
                                global_file_info.total_bytes = total_size;
                                global_file_info.copied_bytes = total_size;
                                global_file_info.progress = 100.0;
                                global_file_info.error_msg = "";
                                global_file_info.warning_msg = warn_str;
                                std::cout << ts() << "[FILE_INFO] "
                                          << (recovering ? "installer now validates, clearing previous failure"
                                                         : "emmcdl.exe found, installer ready")
                                          << (warn_str.empty() ? "" : " (with warning)") << std::endl;
                                if (!warn_str.empty())
                                    std::cout << ts() << "[FILE_INFO WARNING] " << warn_str << std::endl;
                            } else {
                                // 狀態早就是就緒了（上面的 if 不會進去），但缺檔清單
                                // 與 flash_stage 需要的 XML 都會變，警告內容必須每輪
                                // 重新同步，否則會顯示過期的警告。
                                bool changed = false;
                                {
                                    std::lock_guard<std::mutex> lock(file_info_mutex);
                                    if (global_file_info.warning_msg != warn_str) {
                                        global_file_info.warning_msg = warn_str;
                                        changed = true;
                                    }
                                }
                                if (changed && !warn_str.empty())
                                    std::cout << ts() << "[FILE_INFO WARNING] " << warn_str << std::endl;
                            }
                        } else {
                            // 資料夾存在但沒有 emmcdl.exe，需要複製/解壓縮
                            if (current_status != FileStatus::COPY_FAILED &&
                                !is_copy_in_progress(current_status)) {
                                std::lock_guard<std::mutex> lock(file_info_mutex);
                                global_file_info.status = FileStatus::WAITING_FOR_COPY;
                                global_file_info.total_bytes = 0;
                                global_file_info.copied_bytes = 0;
                                global_file_info.progress = 0.0;
                                global_file_info.error_msg = "";
                                global_file_info.warning_msg = "";
                                global_file_info.current_file = 0;
                                global_file_info.total_files = 0;
                                std::cout << ts() << "[FILE_INFO] Folder exists but no emmcdl.exe → WAITING_FOR_COPY" << std::endl;
                            }
                        }
                    } else {
                        // installer path 不存在，無條件設為 NOT_FOUND（允許重試）
                        std::lock_guard<std::mutex> lock(file_info_mutex);
                        global_file_info.status = FileStatus::NOT_FOUND;
                        global_file_info.total_bytes = 0;
                        global_file_info.copied_bytes = 0;
                        global_file_info.progress = 0.0;
                        global_file_info.error_msg = "";
                        std::cout << ts() << "[FILE_INFO] Installer path not found, status = NOT_FOUND" << std::endl;
                    }
                }
            } else {
                // 正在 COPYING，不修改狀態（每 2 秒一次，不印避免高頻寫 stdout）
            }
            
            // 🔧 使用現有 talker 發送，不創建新的
            FileInfo info_to_send;
            {
                std::lock_guard<std::mutex> lock(file_info_mutex);
                info_to_send = global_file_info;
            }

            // 這裡以前有一份手寫的 FileInfo 序列化，跟 send_file_status() 裡那份
            // 重複。兩份各自維護的結果是新增 warning_msg 時只改到其中一份：
            // 警告在 client log 有、UI 卻永遠是空的（因為 server 收到的正是這條
            // 每 2 秒的廣播）。改成直接呼叫同一個序列化函式，欄位只會有一個定義處。
            send_file_status(talker, info_to_send);

            last_file_status_time = now_file;

            auto t_filestatus_end = std::chrono::steady_clock::now();
            auto filestatus_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_filestatus_end - t_filestatus_start).count();
            if (filestatus_ms > 500) {
                std::cout << ts() << "[PERF] file status check+send took " << filestatus_ms << "ms\n";
            }
        }
        

        // ===== Check if need to copy installer =====
        if (auto_flash)
        {
            // 這個區塊要做遞迴目錄掃描 + 讀 XML + 查 zip 內容，成本不低，而
            // com_monitor 迴圈是 100ms 一輪。原本的 copy_conuter % 5 節流是壞的
            // （copy_conuter 每輪都被設回 0，餘數永遠是 0），等於 10Hz 全速跑，
            // 在 38GB／148 檔的真實 installer 上既浪費 I/O 又把 log 洗爆。
            // 改成明確的 2 秒時間節流。
            static auto last_copy_check = std::chrono::steady_clock::time_point{};
            auto now_copy = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    now_copy - last_copy_check).count() >= 2000) {
                last_copy_check = now_copy;
                std::string installer = listener->installer_path;
                std::string download  = listener->download_path;
                copy_conuter = 0;

                // 判斷 download 是 ZIP 還是資料夾（用副檔名）——use_delta
                // 判斷、log 訊息都需要提前知道，故提到 needs_copy 判斷之前。
                auto dl_lower = download;
                std::transform(dl_lower.begin(), dl_lower.end(), dl_lower.begin(), ::tolower);
                bool download_is_zip = dl_lower.size() >= 4 &&
                    dl_lower.rfind(".zip") == dl_lower.size() - 4;

                // installer path 必須設定且是資料夾，且沒有 emmcdl.exe，才需要複製
                if (!installer.empty() && !download.empty() && !copy_running) {
                    // 檢查 installer path 狀態
                    std::filesystem::path inst_path(installer);
                    bool inst_exists = std::filesystem::exists(inst_path);
                    bool inst_is_dir = inst_exists && std::filesystem::is_directory(inst_path);

                    // 如果 installer path 不是資料夾（存在但是檔案），不觸發複製
                    if (inst_exists && !inst_is_dir) {
                        // PATH_NOT_A_FOLDER，不做任何事
                    } else {
                        // installer path 不存在，或是資料夾但缺少燒錄所需的檔案
                        // （emmcdl.exe，或 rawprogram0.xml/WDFlash.xml 及其列出的檔案）
                        bool needs_copy = false;
                        InstallerValidation validation;
                        if (!inst_exists) {
                            needs_copy = true;
                        } else if (inst_is_dir) {
                            validation = validate_installer_files(inst_path, listener->flash_stage, download);
                            needs_copy = !validation.ok;
                        }


                        if (needs_copy) {
                            FileStatus current_status;
                            {
                                std::lock_guard<std::mutex> lock(file_info_mutex);
                                current_status = global_file_info.status;
                            }

                            // 允許觸發複製的狀態。
                            //
                            // FOUND / COPY_COMPLETE 也要放進來：缺檔現在只會被報成
                            // 「success + warning」而不再是 MISSING_FILES，如果只允許
                            // 失敗狀態觸發，delta 補檔就永遠不會啟動了。重複觸發由
                            // 收斂保護把次數收斂掉。
                            // 唯一要排除的是正在進行中（COPYING / UNZIPPING）。
                            if (!is_copy_in_progress(current_status)) {

                                // 先建立 installer path 目錄（不存在時）
                                try {
                                    std::filesystem::create_directories(inst_path);
                                } catch (const std::exception& e) {
                                    std::cout << ts() << "[COPY] Failed to create installer dir: " << e.what() << std::endl;
                                }

                                // 缺檔清單存在，且不是「installer path 整個
                                // 不存在」的第一次部署，才有機會走 delta（只
                                // 補缺檔，不整包重抓）。
                                //
                                // 另外要求「骨架齊備」（emmcdl.exe + 需要的 XML
                                // 都在）：觸發流程會先 create_directories，所以
                                // 全新部署在下一輪就會看到一個空目錄存在，若只
                                // 看 inst_exists 就會對空目錄做 delta，只補
                                // manifest 列到的檔案，留下一個殘缺的 installer。
                                // 骨架不齊 → 整包複製；骨架齊只是少數檔案壞掉
                                // → delta。
                                //
                                // ZIP 來源現在是保留目錄結構解壓的，所以帶路徑
                                // 分隔符號的缺檔名（nor_oob\gpt_main0.bin）在
                                // zip 內有唯一對應的 entry，delta 補檔是明確且
                                // 安全的——不再需要「含分隔符號就退回整包重解」
                                // 的舊 fallback。那個 fallback 正是無限重解迴圈
                                // 的上膛條件：flatten 永遠不建子資料夾，缺檔名
                                // 永遠含分隔符號，於是永遠整包重抓。
                                bool use_delta = inst_exists && inst_is_dir &&
                                                 !validation.missing.empty() &&
                                                 validation.skeleton_ok;

                                // ===== 收斂保護 =====
                                // 沒有這層保護時，只要有一個「來源也給不出來」的
                                // 必要檔案，上面的觸發條件就會無止盡地重跑整包
                                // 複製／解壓（實測跑了 500 輪、每輪從網路磁碟重讀
                                // 44GB）。判定標準是「缺檔清單有沒有變小」：
                                // 複製完一輪之後缺檔集合完全沒改善，代表再重跑
                                // 一次也不會有不同結果，連續兩次沒進展就停手，
                                // 並把真正缺的檔名寫進 error_msg。
                                // 設定（installer/download 路徑）換掉時解除閂鎖，
                                // 使用者修正來源後不用重啟程式。
                                static std::string guard_config;
                                static std::string guard_missing;
                                static int guard_no_progress = 0;
                                static bool guard_latched = false;

                                std::string config_key = installer + "|" + download;
                                std::string missing_key;
                                {
                                    std::vector<std::string> sorted = validation.missing;
                                    std::sort(sorted.begin(), sorted.end());
                                    for (const auto& m : sorted) missing_key += m + ";";
                                }

                                if (config_key != guard_config) {
                                    guard_config = config_key;
                                    guard_missing.clear();
                                    guard_no_progress = 0;
                                    guard_latched = false;
                                    // 換了 installer/download 路徑，舊路徑的空間警告
                                    // 已經不適用（那是對另一顆磁碟／另一份來源算的）。
                                    std::lock_guard<std::mutex> lock(file_info_mutex);
                                    g_space_note.clear();
                                }

                                // 目的地整個不存在了（使用者刪掉重來），等於全新
                                // 部署，先前的「補不好」結論不再適用。
                                if (!inst_exists) {
                                    guard_missing.clear();
                                    guard_no_progress = 0;
                                    guard_latched = false;
                                }

                                // 缺檔集合變了 → 有進展（或換了不同問題），解除閂鎖。
                                if (missing_key != guard_missing) {
                                    guard_no_progress = 0;
                                    guard_latched = false;
                                } else if (last_copy_blocked_by_space.exchange(false)) {
                                    // 空間不足不算無進展，否則清出空間後就再也不會重試。
                                    guard_no_progress = 0;
                                } else if (last_copy_cancelled.exchange(false)) {
                                    // 上一輪是被取消的，不是「跑完卻沒改善」，不計數。
                                    guard_no_progress = 0;
                                } else if (!missing_key.empty()) {
                                    ++guard_no_progress;
                                }
                                guard_missing = missing_key;

                                // 閂鎖／放棄時怎麼回報，取決於「這個目的地到底可不可用」：
                                //   emmcdl.exe 在 → 複製／解壓其實跑完了，只是來源湊
                                //     不出某幾個檔案。依照「完成就算 success，異常用
                                //     warning 說明」的規則，這裡不動狀態，交給
                                //     file-status 檢查回報 FOUND + 一行含缺檔清單的警告。
                                //   emmcdl.exe 不在 → 根本沒產出可用的 installer，
                                //     這才是真正的失敗，而且要明講否則會靜靜卡在
                                //     WAITING_FOR_COPY（實測踩過）。
                                bool have_emmcdl = has_emmcdl(inst_path);

                                // 空間不足是環境問題，依要求只用 warning 呈現，
                                // 絕不因此變成 COPY_FAILED——否則使用者清出空間前
                                // 會看到一個看似程式壞掉的紅字。
                                bool blocked_by_space;
                                {
                                    std::lock_guard<std::mutex> lock(file_info_mutex);
                                    blocked_by_space = !g_space_note.empty();
                                }
                                if (blocked_by_space) have_emmcdl = true;

                                if (guard_latched) {
                                    if (!have_emmcdl) {
                                        std::string joined;
                                        for (size_t i = 0; i < validation.missing.size(); ++i) {
                                            if (i) joined += ", ";
                                            joined += validation.missing[i];
                                        }
                                        std::lock_guard<std::mutex> lock(file_info_mutex);
                                        global_file_info.status = FileStatus::COPY_FAILED;
                                        global_file_info.error_msg =
                                            "Source does not provide required file(s): " + joined +
                                            " (stopped retrying; change installer/download path or"
                                            " delete the destination to retry)";
                                    }
                                } else if (guard_no_progress >= 2) {
                                    guard_latched = true;

                                    std::string joined;
                                    for (size_t i = 0; i < validation.missing.size(); ++i) {
                                        if (i) joined += ", ";
                                        joined += validation.missing[i];
                                    }
                                    std::cout << ts() << "[COPY] *** Giving up: " << guard_no_progress + 1
                                              << " consecutive copies left the same file(s) missing."
                                                 " The source does not provide: " << joined
                                              << ". Not retrying until installer/download path changes. ***\n";

                                    if (!have_emmcdl) {
                                        FileInfo info_to_send;
                                        {
                                            std::lock_guard<std::mutex> lock(file_info_mutex);
                                            global_file_info.status = FileStatus::COPY_FAILED;
                                            global_file_info.error_msg =
                                                "Source does not provide required file(s): " + joined;
                                            info_to_send = global_file_info;
                                        }
                                        send_file_status(talker, info_to_send);
                                    }
                                } else {
                                    // 只有真的要動手時才印來源類型，否則閂鎖之後
                                    // 每輪都印會把 log 洗爆（實測 10Hz 洗頻）。
                                    if (download_is_zip) {
                                        std::cout << ts() << "[COPY] ZIP source detected, will extract to: " << installer << std::endl;
                                    } else {
                                        std::cout << ts() << "[COPY] Folder source detected, will copy to: " << installer << std::endl;
                                    }

                                    copy_cancel = false;
                                    copy_running = true;
                                    static int copy_attempt = 0;
                                    ++copy_attempt;
                                    if (copy_attempt > 1) {
                                        std::string joined;
                                        for (size_t i = 0; i < validation.missing.size(); ++i) {
                                            if (i) joined += ", ";
                                            joined += validation.missing[i];
                                        }
                                        std::cout << ts() << "[COPY] *** This is copy attempt #" << copy_attempt
                                                  << " — still missing: "
                                                  << (joined.empty() ? "(installer path did not exist)" : joined)
                                                  << " ***\n";
                                    }
                                    std::vector<std::string> delta_names =
                                        use_delta ? validation.missing : std::vector<std::string>{};
                                    copy_thread = std::thread([=]() {
                                        copy_worker(download, installer, ip, delta_names);
                                        copy_running = false;
                                    });
                                    copy_thread.detach();
                                }
                            }
                        }
                    }
                }
                copy_conuter = 0;
            }
        }
        else
        {
            copy_cancel = true;
            copy_conuter = 0;
        }

        // 🔍 診斷：單輪迴圈超過 1 秒就代表 heartbeat 一定會遲交（server 端 5 秒判 DEAD）
        {
            auto loop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - loop_start).count();
            if (loop_ms > 1000) {
                std::cout << ts() << "[PERF] com_monitor loop took " << loop_ms << "ms (expected ~100ms)\n";
            }
        }

        // ===== 把「送給遠端的同一份資料」也發佈給本機 WebUI =====
        // 發佈點放在迴圈尾端、所有封包都送完之後，本機看到的就跟遠端收到的一致。
        //
        // 這裡的 ip 顯示要跟遠端 host_server 的邏輯對齊：host_server 顯示的是
        // accept() 看到的來源位址，也就是本機的 IP；如果本機 WebUI 直接照搬
        // mirror.ip = ip（那是「要送去哪個 server」的目標位址，跟本機 IP是
        // 兩個不同東西），就會看起來反過來。改成顯示本機 IP，同時保留
        // target server 的 ip／反解主機名，讓 UI 在下面小字顯示「送去哪台」。
        {
            static std::string cached_local_ip;
            static bool local_ip_ready = false;
            static std::string cached_server_name;
            static bool server_name_tried = false;

            if (!local_ip_ready) {
                std::string resolved = get_local_ip_facing(ip);
                if (!resolved.empty()) {
                    cached_local_ip = resolved;
                    local_ip_ready = true;
                }
            }
            if (!server_name_tried) {
                cached_server_name = get_server_display_name(ip);
                server_name_tried = true;
            }

            LocalMirrorState mirror;
            mirror.ip = local_ip_ready ? cached_local_ip : ip;  // 還沒查到本機 IP 前，先顯示目標 IP 免得空白
            mirror.server_ip = ip;
            mirror.server_name = cached_server_name;
            mirror.device = get_device_name();
            mirror.heartbeat = true;              // 這個迴圈還在跑就代表活著
            mirror.auto_flash = listener->auto_flash();
            mirror.installer_path = listener->installer_path;
            mirror.download_path = listener->download_path;
            mirror.chipset = listener->chipset;
            mirror.storage = clamp_storage(listener->chipset, listener->storage);
            mirror.flash_stage = listener->flash_stage;
            {
                std::lock_guard<std::mutex> lock(file_info_mutex);
                mirror.file_info = global_file_info;
            }
            {
                std::lock_guard<std::mutex> lock(com_mutex);
                for (const auto& [num, info] : comport_map) {
                    LocalMirrorState::Comport c;
                    c.number = num;
                    c.status = info.status;
                    c.log = info.log;
                    c.error_msg = info.error_msg;
                    mirror.comports.push_back(std::move(c));
                }
            }
            local_server_publish(mirror);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    }
}

int main(int argc, char* argv[])
{
    // 🔧 禁用 stdout 緩衝，讓 log 即時輸出
    std::cout.setf(std::ios::unitbuf);
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    init_socket();


    int listen_port = 9000;
    Listener listener(listen_port);
    listener.start();

    // std::string ip = "10.81.95.53";  // USA server
    std::string ip = "10.235.50.244"; // trueforge-bm
    // std::string ip = "100.86.11.122";
    // std::string ip = "192.168.1.106";

    
    // argv[1] exists -> override default
    if (argc >= 2) {
        ip = argv[1];
    }

    // ===== 本機 WebUI / API =====
    // 預設埠 47913：IANA 未指派、也不在常見服務或開發工具的慣用範圍，撞埠機率低。
    // 需要時可用 --local-port 改，或 --no-local 完全關掉。
    int  local_port   = 47913;
    bool open_browser = true;
    bool local_enable = true;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--no-local") {
            local_enable = false;
        } else if (a == "--no-browser") {
            open_browser = false;
        } else if (a == "--local-port" && i + 1 < argc) {
            try { local_port = std::stoi(argv[++i]); } catch (...) {
                std::cout << "[LOCAL] Ignoring bad --local-port value\n";
            }
        }
    }

    if (local_enable) {
        if (!local_server_start(local_port, open_browser)) {
            // 本機 UI 起不來不該讓整個 client 停擺——燒錄本身完全不依賴它。
            std::cout << "[LOCAL] Local WebUI disabled (could not bind port "
                      << local_port << ")\n";
        }
    }


    std::thread com_monitor_thread(com_monitor,ip, &listener);
    com_monitor_thread.detach();    

    

                while (true)
    {
        int comport = -1;

                if (auto_flash)
        {
            // 🔧 先檢查檔案狀態（最重要的檢查）
            FileStatus current_file_status;
            {
                std::lock_guard<std::mutex> lock(file_info_mutex);
                current_file_status = global_file_info.status;
            }
            
            // 如果正在複製，等待完成
            if (is_copy_in_progress(current_file_status)) {
                // ⚠️ 不要持著鎖 sleep：com_monitor 每輪都要拿 file_info_mutex
                //    讀狀態，抱著鎖睡 2 秒會直接拖慢 heartbeat。
                //    另外 new_com_queue 是 com_mutex 保護的，不是 file_info_mutex。
                double progress_snapshot;
                {
                    std::lock_guard<std::mutex> lock(file_info_mutex);
                    progress_snapshot = global_file_info.progress;
                }
                size_t queue_size;
                {
                    std::lock_guard<std::mutex> lock(com_mutex);
                    queue_size = new_com_queue.size();
                }

                // 進度變化不大時不重複印，避免在無緩衝 stdout 上高頻寫入
                // （console 消費端讀得慢時 write() 會阻塞）。
                static double last_logged_progress = -1.0;
                if (progress_snapshot - last_logged_progress >= 1.0 ||
                    last_logged_progress < 0.0) {
                    last_logged_progress = progress_snapshot;
                    std::cout << ts() << "[WAITING] Installer is copying ("
                              << progress_snapshot << "%), "
                              << "Queue has " << queue_size
                              << " COM port(s) waiting...\n";
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                continue;
            }
            
            // 如果檔案不存在或複製失敗，等待
            if (current_file_status != FileStatus::COPY_COMPLETE && 
                current_file_status != FileStatus::FOUND) {
                std::cout << ts() << "[WAITING] Installer not ready (status: " << static_cast<int>(current_file_status) << ")\n";
                std::cout << ts() << "[WAITING] Queue has " << new_com_queue.size() << " COM port(s) waiting for installer...\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                continue;
            }
            
            // 最後確認燒錄所需的檔案都齊全：emmcdl.exe，以及依 flash_stage
            // 決定需要的 rawprogram0.xml/WDFlash.xml 本身跟其中列出的檔案
            // （不分大小寫，遞迴，含所有 subfolder）
            InstallerValidation final_validation =
                validate_installer_files(std::filesystem::path(listener.installer_path), listener.flash_stage, listener.download_path);
            if (!final_validation.ok)
            {
                std::string missing_str;
                for (size_t i = 0; i < final_validation.missing.size(); ++i) {
                    if (i) missing_str += ", ";
                    missing_str += final_validation.missing[i];
                }
                std::cout << ts() << "[WAITING] Missing required files in " << listener.installer_path
                          << ": " << missing_str << std::endl;
                std::cout << ts() << "[WAITING] Queue has " << new_com_queue.size() << " COM port(s) waiting for installer...\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                continue;
            }

            // Installer exists and ready, process queue
            {
                std::unique_lock<std::mutex> lock(com_mutex);
                com_cv.wait(lock, [] {
                    return !new_com_queue.empty();
                });

                // Check auto_flash again after waking up
                if (!auto_flash) {
                    std::cout << ts() << "[MAIN] Auto flash disabled, clearing queue\n";
                    // Clear the queue
                    while (!new_com_queue.empty()) {
                        int skipped = new_com_queue.front();
                        new_com_queue.pop();
                        std::cout << ts() << "[SKIP] COM" << skipped << " (auto flash disabled)\n";
                    }
                    continue;
                }

                                // 🔧 再次檢查檔案狀態（在持有 com_mutex 鎖的情況下）
                FileStatus final_check_status;
                {
                    std::lock_guard<std::mutex> lock2(file_info_mutex);
                    final_check_status = global_file_info.status;
                }
                
                // 如果正在複製或狀態不正確，保持在隊列中
                if (is_copy_in_progress(final_check_status)) {
                    std::cout << ts() << "[WAITING] Installer is still copying, keeping COM ports in queue\n";
                    continue;
                }
                
                if (final_check_status != FileStatus::COPY_COMPLETE && 
                    final_check_status != FileStatus::FOUND) {
                    std::cout << ts() << "[WAITING] Installer not ready (status: " << static_cast<int>(final_check_status) << "), keeping COM ports in queue\n";
                    continue;
                }
                
                // 最後確認燒錄所需的檔案都齊全（emmcdl.exe + rawprogram0.xml/
                // WDFlash.xml 本身跟其中列出的檔案）
                InstallerValidation final_files_check =
                    validate_installer_files(std::filesystem::path(listener.installer_path), listener.flash_stage, listener.download_path);
                if (!final_files_check.ok) {
                    std::string missing_str;
                    for (size_t i = 0; i < final_files_check.missing.size(); ++i) {
                        if (i) missing_str += ", ";
                        missing_str += final_files_check.missing[i];
                    }
                    std::cout << ts() << "[WAITING] Missing required files (" << missing_str
                              << "), keeping COM ports in queue\n";
                    continue;
                }

                comport = new_com_queue.front();
                new_com_queue.pop();
            }

            // Start flashing
            {
                std::lock_guard<std::mutex> lock(com_mutex);
                if (comport_map.find(comport) != comport_map.end())
                {
                    comport_map[comport].status = ComportStatus::FLASHING;
                    comport_map[comport].error_msg.clear();
                    std::cout << ts() << "[MAIN] Start flashing COM" << comport << " [FLASHING]\n";

                    // emmcdl.exe 直接在 installer_path 裡
                    std::string flash_folder = listener.installer_path;

                    // 讀取這台 client 目前的 chipset / storage / stage 設定
                    Chipset chipset = listener.chipset;
                    StorageType storage = clamp_storage(chipset, listener.storage);
                    FlashStage stage = listener.flash_stage;

                    std::cout << ts() << "[MAIN] Flash config: chipset="
                              << chipset_to_string(chipset)
                              << ", storage=" << storage_to_string(storage)
                              << ", stage=" << stage_to_string(stage) << "\n";

                    std::thread flash_thread(
                        flash_worker,
                        flash_folder,
                        comport,
                        chipset,
                        storage,
                        stage
                    );
                    flash_thread.detach();
                }
                else
                {
                    std::cout << ts() << "[SKIP] COM not in comport_map: " << comport << "\n";
                }
            }
        }
        else
        {
            // Auto flash is disabled
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    
    


    
    return 1;
}