#include "utils/zip/unzip.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <iomanip>
#include <string>
#include <sstream>
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <mutex>
#include <atomic>
#include <unordered_map>

#ifdef _WIN32
    #include <windows.h>
    #include <shlwapi.h>
    #pragma comment(lib, "shlwapi.lib")
#endif

// Simple ZIP file signature check
bool is_zip_file(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    // Check for ZIP signature: PK\x03\x04
    char signature[4];
    file.read(signature, 4);

    return (signature[0] == 'P' &&
            signature[1] == 'K' &&
            signature[2] == 0x03 &&
            signature[3] == 0x04);
}

namespace {

std::string to_lower_str(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

// 執行一次性、不需要即時進度的指令，同步讀完全部 stdout 後回傳。只用於
// get_zip_entry_sizes 這種短命令（列出 zip 內容），不適合拿來跑會卡很久的
// 解壓縮（那種需要 unzip_file 既有的 pipe + 逐行解析進度機制）。
std::string run_capture(const std::string& command)
{
    std::string out;
#ifdef _WIN32
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) return out;

    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) out += buf;

#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return out;
}

struct ZipSizeCacheEntry {
    std::filesystem::file_time_type mtime;
    std::unordered_map<std::string, uint64_t> sizes;
};

// 把一個 zip entry 的大小登記進表裡，同時建立兩種 key：
//   1) 正規化成反斜線的「zip 內相對路徑」（nor_oob\gpt_main0.bin）——精確 key，
//      用 operator[] 覆寫式寫入。相對路徑本身唯一，覆寫是安全的，而且能修正
//      下面那個 first-wins 的純檔名 key：根目錄檔案的相對路徑就等於它的檔名，
//      若某個子資料夾的同名 entry 剛好排在前面先佔走了檔名 key，這一步會把它
//      改回根目錄那一份的大小（manifest 裡的純檔名指的就是根目錄那一份）。
//   2) 純檔名（gpt_main0.bin）——退路，first-wins。
void add_zip_entry_size(std::unordered_map<std::string, uint64_t>& sizes,
                        const std::string& raw_full_name,
                        uint64_t sz)
{
    std::string full = raw_full_name;
    std::replace(full.begin(), full.end(), '/', '\\');
    if (full.empty() || full.back() == '\\') return;  // 目錄項不算檔案

    size_t slash = full.rfind('\\');
    std::string base = (slash == std::string::npos) ? full : full.substr(slash + 1);
    if (base.empty()) return;

    sizes.emplace(to_lower_str(base), sz);
    sizes[to_lower_str(full)] = sz;
}

std::mutex g_zip_size_cache_mutex;
std::unordered_map<std::string, ZipSizeCacheEntry> g_zip_size_cache;

// PowerShell 單引號字串跳脫：' -> ''。路徑是伺服器端傳來的，未跳脫的單引號會
// 讓後面的腳本內容跳出字串常值。
std::string ps_quote(const std::string& s)
{
    std::string out;
    for (char c : s) {
        if (c == '\'') out += "''";
        else out += c;
    }
    return out;
}

// 正規化成 Windows 形式的絕對路徑，且不破壞 UNC 路徑。
// std::filesystem::absolute() 在某些 MinGW 實作會把 \\server\share 誤判成相對
// 路徑並加上當前目錄前綴——生產環境的 installer zip 正是放在 UNC 網路磁碟上
// （\\trueforge-bm\workspace\...），所以 UNC 一律原樣使用。
std::string win_abs_path(const std::string& p)
{
    std::string s = p;
    std::replace(s.begin(), s.end(), '/', '\\');
    if (s.size() >= 2 && s[0] == '\\' && s[1] == '\\') return s;   // UNC

    std::error_code ec;
    std::string a = std::filesystem::absolute(s, ec).string();
    if (ec) return s;
    std::replace(a.begin(), a.end(), '/', '\\');
    return a;
}

// POSIX shell 單引號跳脫：把字串包進 '...'，內含的 ' 換成 '\''。
// zip/dest 路徑是伺服器端傳來的，直接內插進 system()/popen() 的命令字串裡，
// 雙引號內的 $( )、` ` 仍然會被 shell 展開。
std::string sh_quote(const std::string& s)
{
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::atomic<unsigned> g_ps_temp_seq{0};

} // namespace

std::unordered_map<std::string, uint64_t> get_zip_entry_sizes(const std::string& zip_path)
{
    std::error_code ec;
    auto mtime = std::filesystem::last_write_time(zip_path, ec);
    if (ec) return {};

    {
        std::lock_guard<std::mutex> lock(g_zip_size_cache_mutex);
        auto it = g_zip_size_cache.find(zip_path);
        if (it != g_zip_size_cache.end() && it->second.mtime == mtime)
            return it->second.sizes;
    }

    std::unordered_map<std::string, uint64_t> sizes;

#ifdef _WIN32
    std::string abs_zip_str = win_abs_path(zip_path);

    std::string ps_script =
        "Add-Type -AssemblyName System.IO.Compression.FileSystem; "
        "$z = [System.IO.Compression.ZipFile]::OpenRead('" + ps_quote(abs_zip_str) + "'); "
        "foreach ($e in $z.Entries) { Write-Output ($e.FullName + '|' + $e.Length) }; "
        "$z.Dispose();";

    // 檔名帶 pid + 序號：這個函式會被輪詢式地反覆呼叫，固定檔名在併發時會互相覆蓋。
    std::filesystem::path temp_script =
        std::filesystem::temp_directory_path() /
        ("af_zip_sizes_" + std::to_string(GetCurrentProcessId()) + "_" +
         std::to_string(g_ps_temp_seq.fetch_add(1)) + ".ps1");
    {
        std::ofstream f(temp_script);
        f << ps_script;
    }

    std::string cmd = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File \""
                     + temp_script.string() + "\"";
    std::string output = run_capture(cmd);
    std::filesystem::remove(temp_script, ec);

    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t sep = line.rfind('|');
        if (sep == std::string::npos) continue;

        std::string name = line.substr(0, sep);
        uint64_t sz = 0;
        try { sz = std::stoull(line.substr(sep + 1)); } catch (...) { continue; }

        add_zip_entry_size(sizes, name, sz);
    }
#else
    std::string output = run_capture("unzip -l " + sh_quote(zip_path));

    std::istringstream iss(output);
    std::string line;
    bool in_data = false;
    while (std::getline(iss, line)) {
        // "unzip -l" 輸出：header 之後接一行 "----" 分隔線、資料行，再接一行
        // "----" 分隔線跟總計行。用分隔線切換 in_data，剛好只保留資料行。
        if (line.find("----") != std::string::npos) { in_data = !in_data; continue; }
        if (!in_data) continue;

        std::istringstream ls(line);
        uint64_t sz = 0;
        std::string date, time_str;
        if (!(ls >> sz >> date >> time_str)) continue;

        std::string name;
        std::getline(ls, name);
        size_t start = name.find_first_not_of(' ');
        if (start == std::string::npos) continue;
        name = name.substr(start);

        add_zip_entry_size(sizes, name, sz);
    }
#endif

    {
        std::lock_guard<std::mutex> lock(g_zip_size_cache_mutex);
        g_zip_size_cache[zip_path] = ZipSizeCacheEntry{mtime, sizes};
    }
    return sizes;
}

#ifdef _WIN32

// Windows implementation using PowerShell with progress
bool unzip_file(const std::string& zip_path,
                const std::string& dest_path,
                bool flatten,
                UnzipProgressCallback progress_callback,
                std::string* error_msg,
                const std::vector<std::string>& only_names,
                const std::atomic<bool>* cancel,
                bool* out_insufficient_space)
{
    std::cout << "[UNZIP] Starting to extract: " << zip_path << std::endl;
    std::cout << "[UNZIP] Destination: " << dest_path << std::endl;
    std::cout << "[UNZIP] Flatten: " << (flatten ? "yes" : "no") << std::endl;
    if (!only_names.empty()) {
        std::cout << "[UNZIP] Delta extract: " << only_names.size() << " requested file(s)" << std::endl;
    }

    // Create destination directory
    std::filesystem::create_directories(dest_path);

    // 🔧 UNC 路徑處理（\\server\share\...）：見 win_abs_path 的說明，
    // absolute() 會破壞 UNC，而生產環境的 installer zip 正是放在 UNC 上。
    std::filesystem::path abs_dest = std::filesystem::absolute(dest_path);

    std::string abs_zip_str  = win_abs_path(zip_path);
    std::string abs_dest_str = win_abs_path(dest_path);

    std::cout << "[UNZIP] Resolved zip path:  " << abs_zip_str << std::endl;
    std::cout << "[UNZIP] Resolved dest path: " << abs_dest_str << std::endl;

    // only_names 比對用小寫、分隔符號正規化成反斜線的字串（Windows 檔名不分
    // 大小寫，XML 裡的 filename="..." 跟 zip entry 實際大小寫／斜線方向不一定
    // 一致）。腳本裡會同時拿「zip 內相對路徑」跟「純檔名」兩種形式去比對，所以
    // nor_oob\gpt_main0.bin 跟 emmcdl.exe 兩種寫法都能命中。
    // 空清單代表不篩選，維持全量解壓的既有行為。
    std::string wanted_list;
    for (const auto& name : only_names) {
        std::string escaped = to_lower_str(name);
        std::replace(escaped.begin(), escaped.end(), '/', '\\');
        // PowerShell 單引號字串跳脫：把 ' 換成 ''
        std::string out;
        for (char c : escaped) {
            if (c == '\'') out += "''";
            else out += c;
        }
        if (!wanted_list.empty()) wanted_list += ",";
        wanted_list += "'" + out + "'";
    }
    // 統一的解壓腳本，flatten 與結構保留共用同一套路徑計算與進度統計。
    //
    // 為什麼要自己算「輸出相對路徑」而不是直接用 $entry.FullName／$entry.Name：
    //   1) 實務上遇到的 installer zip 內部分隔符號是「反斜線」而不是 ZIP 規格
    //      規定的正斜線（例如 nor_oob\gpt_main0.bin）。所有以 '/' 為前提的判斷
    //      （'*/' 目錄過濾、.NET 的 $entry.Name 推導）在這種 zip 上都會失準，
    //      所以先把分隔符號正規化成反斜線，再自己切出 basename。
    //   2) flatten 模式只用 basename 當輸出路徑，會讓不同子資料夾裡的同名檔案
    //      互相覆蓋——連 rawprogram0.xml 這種 manifest 都會被子資料夾的變體蓋掉，
    //      產生「看起來有檔案但內容是錯變體」的壞 installer。結構保留模式把
    //      $rel 設成完整相對路徑，子資料夾會被真的建出來。
    //   3) 進度統計改成先用「輸出相對路徑」去重（後者覆蓋前者，跟實際落地行為
    //      一致）再加總，所以 TOTAL_BYTES 永遠等於真正會寫到磁碟的位元組數，
    //      不會被重複檔名重複計算而虛胖（虛胖會讓進度%跟實際大小對不上）。
    std::string ps_script =
        "Add-Type -AssemblyName System.IO.Compression.FileSystem\n"
        "$ErrorActionPreference = 'Stop'\n"
        "$wanted = @(" + wanted_list + ")\n"
        "$flatten = $" + std::string(flatten ? "true" : "false") + "\n"
        "$zip = [System.IO.Compression.ZipFile]::OpenRead('" + ps_quote(abs_zip_str) + "')\n"
        "$dest = '" + ps_quote(abs_dest_str) + "'\n"
        "$sel = New-Object System.Collections.Specialized.OrderedDictionary\n"
        "$cand = New-Object System.Collections.ArrayList\n"
        "foreach ($e in $zip.Entries) {\n"
        "  $full = $e.FullName.Replace('/','\\')\n"
        "  if ([string]::IsNullOrWhiteSpace($full)) { continue }\n"
        "  if ($full.EndsWith('\\')) { continue }\n"
        "  $base = $full.Substring($full.LastIndexOf('\\') + 1)\n"
        "  if ([string]::IsNullOrEmpty($base)) { continue }\n"
        "  if ($flatten) { $rel = $base } else { $rel = $full }\n"
        "  $bad = $false\n"
        "  foreach ($seg in $rel.Split('\\')) { if ($seg -eq '..' -or $seg -eq '.') { $bad = $true } }\n"
        "  if ($bad -or $rel.StartsWith('\\') -or $rel.Contains(':')) { Write-Host \"SKIP:$full\"; continue }\n"
        "  [void]$cand.Add(@{ E = $e; R = $rel; F = $full.ToLower(); B = $base.ToLower() })\n"
        "}\n"
        // only_names 比對分兩輪：先收「zip 內相對路徑完全相符」的 entry，剩下
        // 還沒對到的名字才用純檔名去找（取第一個）。順序很重要：像
        // gpt_main0.bin 這種純檔名在多個 NOR 變體子資料夾都有同名檔案，一律用
        // 檔名比對會一次撈出全部變體，補檔範圍比要求的大；先比精確路徑就能把
        // 「manifest 裡的純檔名指的是根目錄那一份」這個語意正確表達出來。
        "if ($wanted.Count -gt 0) {\n"
        "  $hit = @{}\n"
        "  foreach ($c in $cand) { if ($wanted -contains $c.F) { $sel[$c.R.ToLower()] = $c; $hit[$c.F] = $true } }\n"
        "  foreach ($w in $wanted) {\n"
        "    if ($hit.ContainsKey($w)) { continue }\n"
        "    foreach ($c in $cand) {\n"
        "      if ($c.B -eq $w) { $sel[$c.R.ToLower()] = $c; $hit[$w] = $true; break }\n"
        "    }\n"
        "  }\n"
        "} else {\n"
        "  foreach ($c in $cand) { $sel[$c.R.ToLower()] = $c }\n"
        "}\n"
        "$items = @($sel.Values)\n"
        // 目的地已經有一份大小相同的檔案就跳過不重解。
        //
        // 這是「續傳」：以前中斷後下一輪會把整包重解一次（實測 44GB 要 6 分鐘以上），
        // 已經寫好的檔案全部白做。判準用大小相同而不是逐位元組比對，理由與 folder
        // 複製那邊一致（整包幾十 GB，讀完再比會比重解還慢），也跟
        // validate_installer_files 判斷完整性的標準相同。
        "$todo = New-Object System.Collections.ArrayList\n"
        "$skipCount = 0\n"
        "$skipBytes = [int64]0\n"
        "foreach ($i in $items) {\n"
        "  $p = Join-Path $dest $i.R\n"
        "  if (Test-Path -LiteralPath $p) {\n"
        "    $fi = Get-Item -LiteralPath $p -Force\n"
        "    if (-not $fi.PSIsContainer -and $fi.Length -eq $i.E.Length) {\n"
        "      $skipCount++; $skipBytes += [int64]$i.E.Length; continue\n"
        "    }\n"
        "  }\n"
        "  [void]$todo.Add($i)\n"
        "}\n"
        "if ($skipCount -gt 0) { Write-Host \"SKIPPED_EXISTING:${skipCount}:${skipBytes}\" }\n"
        "$items = @($todo)\n"
        "$totalFiles = $items.Count\n"
        "$totalBytes = [int64]0\n"
        "foreach ($i in $items) { $totalBytes += [int64]$i.E.Length }\n"
        "Write-Host \"TOTAL_FILES:$totalFiles\"\n"
        "Write-Host \"TOTAL_BYTES:$totalBytes\"\n"
        "$currentFile = 0\n"
        "$currentBytes = [int64]0\n"
        "foreach ($i in $items) {\n"
        "  $currentFile++\n"
        "  $destFile = Join-Path $dest $i.R\n"
        "  $destDir = Split-Path $destFile -Parent\n"
        "  if (!(Test-Path $destDir)) { New-Item -ItemType Directory -Path $destDir -Force | Out-Null }\n"
        "  [System.IO.Compression.ZipFileExtensions]::ExtractToFile($i.E, $destFile, $true)\n"
        "  $currentBytes += [int64]$i.E.Length\n"
        "  Write-Host \"PROGRESS:${currentFile}:${totalFiles}:${currentBytes}:${totalBytes}:$($i.R)\"\n"
        "}\n"
        "$zip.Dispose()\n"
        "Write-Host 'COMPLETE'\n";

    // 暫存腳本放系統 temp 目錄，不要放在解壓目的地裡面。放在目的地會有兩個
    // 問題：(1) 它是個不屬於 installer 的多餘檔案，會被
    // validate_installer_files／磁碟大小輪詢一起算進去；(2) 兩個解壓同時跑時
    // 會互相覆蓋腳本。檔名帶上 process id + 遞增序號避免碰撞。
    static std::atomic<unsigned> unzip_seq{0};
    std::string temp_script =
        (std::filesystem::temp_directory_path() /
         ("af_unzip_" + std::to_string(GetCurrentProcessId()) + "_" +
          std::to_string(unzip_seq.fetch_add(1)) + ".ps1")).string();

    std::ofstream script_file(temp_script);
    if (!script_file) {
        std::cout << "[UNZIP ERROR] Failed to create temp script file" << std::endl;
        return false;
    }
    script_file << ps_script;
    script_file.close();

    // 解壓前先量一次目的地已有的大小當基準線。磁碟輪詢的進度要用「成長量」
    // 而不是「資料夾總大小」——delta 解壓時目的地本來就已經幾乎是滿的，
    // 直接拿總大小去除 TOTAL_BYTES（只有幾個缺檔的大小）會算出遠超 100% 的
    // 假進度。
    uint64_t baseline_bytes = 0;
    {
        std::error_code bec;
        if (std::filesystem::exists(abs_dest, bec)) {
            for (std::filesystem::recursive_directory_iterator
                     it(abs_dest, std::filesystem::directory_options::skip_permission_denied, bec), end;
                 !bec && it != end; it.increment(bec)) {
                std::error_code fec;
                if (it->is_regular_file(fec)) baseline_bytes += it->file_size(fec);
            }
        }
    }

    // Execute PowerShell script with output capture
    std::string ps_command = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + temp_script + "\"";

    std::cout << "[UNZIP] Executing extraction..." << std::endl;

    // Create pipe for output
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead, hWrite;
    CreatePipe(&hRead, &hWrite, &sa, 0);
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};

    BOOL success = CreateProcessA(
        nullptr,
        const_cast<char*>(ps_command.c_str()),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi
    );

    CloseHandle(hWrite);

    if (!success) {
        std::cout << "[UNZIP ERROR] Failed to start PowerShell" << std::endl;
        CloseHandle(hRead);
        std::filesystem::remove(temp_script);
        return false;
    }

    // Read output and parse progress
    int total_files = 0;
    uint64_t total_bytes = 0;
    int current_file = 0;
    uint64_t current_bytes = 0;
    bool extraction_complete = false;
    bool has_error = false;
    bool child_exited = false;
    DWORD child_exit_code = 0;

    char buffer[4096];
    DWORD bytesRead;
    std::string output_buffer;

    // 🔧 定期用實際磁碟已寫入大小回報進度，不要只靠 PowerShell 整個
    // entry 解壓完才印的 PROGRESS 行——單一大檔案解壓時，
    // ExtractToFile() 中途完全不會有輸出，導致 progress 卡住不動。
    auto last_size_poll = std::chrono::steady_clock::now();
    constexpr auto SIZE_POLL_INTERVAL = std::chrono::milliseconds(1000);

    while (true) {
        if (PeekNamedPipe(hRead, nullptr, 0, nullptr, &bytesRead, nullptr) && bytesRead > 0) {
            ReadFile(hRead, buffer, sizeof(buffer) - 1, &bytesRead, nullptr);
            buffer[bytesRead] = '\0';
            output_buffer += buffer;

            // Process complete lines
            size_t pos;
            while ((pos = output_buffer.find('\n')) != std::string::npos) {
                std::string line = output_buffer.substr(0, pos);
                output_buffer.erase(0, pos + 1);

                // Remove \r if present
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }

                // 🔧 精確偵測 PowerShell 錯誤，避免誤判含有 "Error" 的檔名或路徑
                // PowerShell 錯誤訊息的特徵：以 "Exception"、"At line:" 開頭，
                // 或包含 "CategoryInfo"、"FullyQualifiedErrorId" 等關鍵字
                bool line_is_ps_error = false;
                if (line.find("Exception") != std::string::npos) {
                    line_is_ps_error = true;
                } else if (line.find("Cannot open") != std::string::npos ||
                           line.find("Access is denied") != std::string::npos ||
                           line.find("UnauthorizedAccessException") != std::string::npos) {
                    line_is_ps_error = true;
                } else if (line.find("Error") != std::string::npos) {
                    // 只有在看起來像 PowerShell 錯誤訊息時才標記
                    if (line.find("At line:") != std::string::npos ||
                        line.find("CategoryInfo") != std::string::npos ||
                        line.find("FullyQualifiedErrorId") != std::string::npos ||
                        line.find("TerminatingError") != std::string::npos ||
                        (line.size() > 0 && line.rfind("Error", 0) == 0)) {
                        line_is_ps_error = true;
                    }
                }

                if (line_is_ps_error) {
                    std::cout << "[UNZIP ERROR] " << line << std::endl;
                    has_error = true;
                    // 🔧 收集錯誤訊息供呼叫端使用
                    if (error_msg) {
                        if (!error_msg->empty()) *error_msg += "\n";
                        *error_msg += line;
                    }
                }

                // Parse progress line
                // 🔧 stoi/stoull throw std::invalid_argument on empty/non-numeric
                // input, which would otherwise be an uncaught exception on this
                // thread and take down the whole process (std::terminate) — e.g.
                // PowerShell's Measure-Object emits $null (empty string here) for
                // TOTAL_BYTES when a delta filter matches zero entries. Malformed
                // lines are logged and skipped instead of crashing.
                if (line.find("TOTAL_FILES:") == 0) {
                    try {
                        total_files = std::stoi(line.substr(12));
                        std::cout << "[UNZIP] Total files: " << total_files << std::endl;
                    } catch (const std::exception& e) {
                        std::cout << "[UNZIP] Failed to parse TOTAL_FILES line: \"" << line << "\" (" << e.what() << ")" << std::endl;
                    }
                }
                else if (line.find("TOTAL_BYTES:") == 0) {
                    try {
                        total_bytes = std::stoull(line.substr(12));
                        // MB 是給人看的，精確位元組數才能拿來核對「回報的總量
                        // 等於真正寫到磁碟的位元組數」這個不變式。
                        std::cout << "[UNZIP] Total size: " << (total_bytes / 1024.0 / 1024.0)
                                  << " MB (exact bytes: " << total_bytes << ")" << std::endl;

                        // 空間預檢。這個時點最準：total_bytes 已經扣掉續傳跳過的
                        // 檔案，而且腳本還沒開始寫任何 entry，中止不會留下半截檔案。
                        // 空間不夠就直接收手，交由呼叫端當成 warning 呈現。
                        if (total_bytes > 0) {
                            // std::filesystem::space() 在這個 MinGW 上會回報
                            // available = (uintmax_t)-1 而且不設 error_code，等於
                            // 「查不到」卻裝作成功，空間檢查因此完全失效（實測踩到）。
                            // 改用 Win32 原生 API，它也支援 UNC 路徑。
                            uint64_t avail_bytes = 0;
                            bool have_space_info = false;
                            {
                                ULARGE_INTEGER avail{}, total_b{}, freeb{};
                                std::wstring probe = abs_dest.wstring();
                                if (!probe.empty() && probe.back() != L'\\') probe += L'\\';
                                if (GetDiskFreeSpaceExW(probe.c_str(), &avail, &total_b, &freeb)) {
                                    avail_bytes = static_cast<uint64_t>(avail.QuadPart);
                                    have_space_info = true;
                                }
                            }
                            const uint64_t margin = 256ull * 1024 * 1024;
                            std::cout << "[UNZIP] Space check: need "
                                      << (total_bytes + margin) << " bytes, available "
                                      << (have_space_info ? std::to_string(avail_bytes)
                                                          : std::string("<query failed>"))
                                      << std::endl;
                            if (have_space_info && avail_bytes < total_bytes + margin) {
                                std::cout << "[UNZIP] Not enough disk space: needs "
                                          << ((total_bytes + margin) / 1024.0 / 1024.0 / 1024.0)
                                          << " GB, only " << (avail_bytes / 1024.0 / 1024.0 / 1024.0)
                                          << " GB free - aborting before writing anything"
                                          << std::endl;
                                if (out_insufficient_space) *out_insufficient_space = true;
                                if (error_msg) {
                                    // 數字在這裡才是準的（total_bytes 已扣掉續傳跳過的
                                    // 檔案），所以由這裡產生完整訊息，呼叫端直接沿用，
                                    // 不要再自己算一份——重算過的那版實測印出 0.00 GB。
                                    std::ostringstream o;
                                    o << std::fixed << std::setprecision(2)
                                      << "Not enough disk space: needs "
                                      << ((total_bytes + margin) / 1024.0 / 1024.0 / 1024.0)
                                      << " GB, only " << (avail_bytes / 1024.0 / 1024.0 / 1024.0)
                                      << " GB free (short by "
                                      << ((total_bytes + margin - avail_bytes) / 1024.0 / 1024.0 / 1024.0)
                                      << " GB). Nothing was extracted; free up space and it will "
                                         "continue automatically.";
                                    *error_msg = o.str();
                                }
                                TerminateProcess(pi.hProcess, 1);
                                WaitForSingleObject(pi.hProcess, 5000);
                                CloseHandle(hRead);
                                CloseHandle(pi.hProcess);
                                CloseHandle(pi.hThread);
                                std::error_code rec;
                                std::filesystem::remove(temp_script, rec);
                                return false;
                            }
                        }

                        // 立刻把正確的總量推給呼叫端。呼叫端在解壓開始前只能拿到
                        // 壓縮檔大小（跟解壓後的總量可能差幾百倍），不先更新的話
                        // UI 會拿錯誤的分母算進度，直到第一個 entry 解壓完為止。
                        if (progress_callback) progress_callback(0, total_files, 0, total_bytes);
                    } catch (const std::exception& e) {
                        std::cout << "[UNZIP] Failed to parse TOTAL_BYTES line: \"" << line << "\" (" << e.what() << ")" << std::endl;
                    }
                }
                else if (line.find("PROGRESS:") == 0) {
                    // Parse: PROGRESS:currentFile:totalFiles:currentBytes:totalBytes:filename
                    std::string data = line.substr(9);
                    size_t p1 = data.find(':');
                    size_t p2 = data.find(':', p1 + 1);
                    size_t p3 = data.find(':', p2 + 1);
                    size_t p4 = data.find(':', p3 + 1);

                    if (p1 != std::string::npos && p2 != std::string::npos &&
                        p3 != std::string::npos && p4 != std::string::npos) {
                      try {
                        current_file = std::stoi(data.substr(0, p1));
                        // 🔧 disk-size polling below may already have reported a
                        // higher transient value (partial bytes of the file that
                        // was still being written) — never let the authoritative
                        // per-entry count move progress backwards.
                        current_bytes = std::max(current_bytes, std::stoull(data.substr(p2 + 1, p3 - p2 - 1)));
                        // 回報的進度永遠不超過這次宣告要寫的總量。
                        if (total_bytes > 0 && current_bytes > total_bytes) current_bytes = total_bytes;
                        std::string filename = data.substr(p4 + 1);

                        double progress = total_bytes > 0 ? (double)current_bytes / total_bytes * 100.0 : 0.0;

                        std::cout << "[UNZIP] Progress: " << current_file << "/" << total_files
                                  << " files (" << std::fixed << std::setprecision(1) << progress << "%) - "
                                  << filename << std::endl;

                        // Call progress callback
                        if (progress_callback) {
                            progress_callback(current_file, total_files, current_bytes, total_bytes);
                        }
                      } catch (const std::exception& e) {
                          std::cout << "[UNZIP] Failed to parse PROGRESS line: \"" << line << "\" (" << e.what() << ")" << std::endl;
                      }
                    }
                }
                else if (line.find("SKIPPED_EXISTING:") == 0) {
                    // 續傳：目的地已經有同樣大小的檔案，這次不重解。
                    std::string data = line.substr(17);
                    size_t sep = data.find(':');
                    if (sep != std::string::npos) {
                        std::cout << "[UNZIP] Resuming: " << data.substr(0, sep)
                                  << " file(s) already present ("
                                  << data.substr(sep + 1) << " bytes), skipped" << std::endl;
                    }
                }
                else if (line.find("SKIP:") == 0) {
                    // 腳本因為路徑不安全（.. / 絕對路徑 / 帶磁碟代號）而跳過的
                    // entry。靜默跳過會讓「少了檔案」變成無法診斷的謎題，明確記錄。
                    std::cout << "[UNZIP] Skipped unsafe zip entry: " << line.substr(5) << std::endl;
                }
                else if (line == "COMPLETE") {
                    std::cout << "[UNZIP] Extraction completed successfully" << std::endl;
                    extraction_complete = true;
                }
            }
        }

        // 🔧 定期輪詢目的資料夾實際大小，讓大檔案解壓中途也能持續更新
        // progress（不用等這個 entry 的 ExtractToFile 整個做完）。
        //
        // 用「相對 baseline 的成長量」而不是資料夾總大小，並且夾在 total_bytes
        // 以內：delta 解壓時目的地本來就快滿了，用總大小會算出遠超 100% 的假
        // 進度；夾上限則保證回報的進度永遠不會超過這次真的要寫的位元組數。
        auto now = std::chrono::steady_clock::now();
        if (now - last_size_poll >= SIZE_POLL_INTERVAL) {
            last_size_poll = now;

            if (progress_callback && total_bytes > 0 && !extraction_complete) {
                std::error_code pec;
                uint64_t disk_bytes = 0;
                for (std::filesystem::recursive_directory_iterator
                         it(abs_dest, std::filesystem::directory_options::skip_permission_denied, pec), end;
                     !pec && it != end; it.increment(pec)) {
                    std::error_code fec;
                    if (it->is_regular_file(fec)) disk_bytes += it->file_size(fec);
                }

                uint64_t grown = disk_bytes > baseline_bytes ? disk_bytes - baseline_bytes : 0;
                if (grown > total_bytes) grown = total_bytes;

                if (grown > current_bytes) {
                    current_bytes = grown;
                    double progress = (double)current_bytes / total_bytes * 100.0;
                    std::cout << "[UNZIP] Progress (disk poll): "
                              << std::fixed << std::setprecision(1) << progress << "%" << std::endl;
                    progress_callback(current_file, total_files, current_bytes, total_bytes);
                }
            }
        }

        // 使用者要求取消（例如把 auto_flash 關掉）。子行程不殺掉的話，44GB 的
        // 解壓一旦開始就完全停不下來，只能等它跑完或砍掉整個 client。
        if (cancel && cancel->load()) {
            std::cout << "[UNZIP] Cancel requested, terminating extraction" << std::endl;
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(hRead);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            std::error_code rec;
            std::filesystem::remove(temp_script, rec);
            if (error_msg) *error_msg = "Extraction cancelled";
            return false;
        }

        // Check if process exited
        DWORD exitCode;
        if (!child_exited && GetExitCodeProcess(pi.hProcess, &exitCode) &&
            exitCode != STILL_ACTIVE) {
            child_exited = true;
            child_exit_code = exitCode;
        }

        if (child_exited) {
            // 子行程結束後不能立刻 break：COMPLETE 那行常常還躺在 pipe buffer
            // 裡沒被讀走，直接收工會把一次成功的解壓誤判成失敗
            // （進而觸發清理，把目的地資料夾整個刪掉）。要先把 pipe 抽乾。
            DWORD avail = 0;
            if (!PeekNamedPipe(hRead, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) {
                if (child_exit_code != 0) {
                    std::cout << "[UNZIP ERROR] PowerShell exited with code: "
                              << child_exit_code << std::endl;
                    has_error = true;
                }
                break;
            }
            continue;   // 還有資料，回去讀
        }

        Sleep(50);
    }

    CloseHandle(hRead);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // Clean up temp script
    std::filesystem::remove(temp_script);

    if (has_error || !extraction_complete) {
        std::cout << "[UNZIP] Extraction FAILED" << std::endl;
        // 🔧 如果沒有收集到具體錯誤，提供通用訊息
        if (error_msg && error_msg->empty()) {
            *error_msg = extraction_complete ? "PowerShell reported errors during extraction"
                                             : "Extraction did not complete (process may have crashed)";
        }
        return false;
    }

    std::cout << "[UNZIP] Extraction finished successfully" << std::endl;
    return true;
}

#else

// Linux implementation using system unzip command
bool unzip_file(const std::string& zip_path,
                const std::string& dest_path,
                bool flatten,
                UnzipProgressCallback progress_callback,
                std::string* error_msg,
                const std::vector<std::string>& only_names,
                const std::atomic<bool>* cancel,
                bool* out_insufficient_space)
{
    std::cout << "[UNZIP] Starting to extract: " << zip_path << std::endl;
    std::cout << "[UNZIP] Destination: " << dest_path << std::endl;

    // Create destination directory
    std::filesystem::create_directories(dest_path);

    // unzip 的成員參數比對的是 zip 內部完整路徑，不是 basename——只有在
    // only_names 全部是不含子資料夾的 basename 時（呼叫端已保證），才能先
    // 列出所有內部路徑，反查對應的那一個，再用完整內部路徑當成員參數。
    std::vector<std::string> members;
    if (!only_names.empty()) {
        std::string listing = run_capture("unzip -Z1 \"" + zip_path + "\"");

        std::vector<std::string> internal_paths;
        std::istringstream iss(listing);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) internal_paths.push_back(line);
        }

        for (const auto& want : only_names) {
            std::string want_lower = to_lower_str(want);
            bool found = false;
            for (const auto& p : internal_paths) {
                if (to_lower_str(std::filesystem::path(p).filename().string()) == want_lower) {
                    members.push_back(p);
                    found = true;
                    break;
                }
            }
            if (!found) {
                std::cout << "[UNZIP] Delta extract: \"" << want << "\" not found in archive, skipping" << std::endl;
            }
        }

        std::cout << "[UNZIP] Delta extract: " << members.size() << "/" << only_names.size()
                  << " requested file(s) found in archive" << std::endl;

        if (members.empty()) {
            // 沒有東西可解壓不算失敗——讓上層用目的地存在性檢查判斷「來源也沒有」。
            std::cout << "[UNZIP] Delta extract: nothing to extract" << std::endl;
            return true;
        }
    }

    std::string command;
    if (flatten) {
        // Extract all files to dest without directory structure
        command = "unzip -j -o " + sh_quote(zip_path) + " -d " + sh_quote(dest_path);
    } else {
        // Preserve directory structure
        command = "unzip -o " + sh_quote(zip_path) + " -d " + sh_quote(dest_path);
    }
    for (const auto& m : members) {
        command += " " + sh_quote(m);
    }

    std::cout << "[UNZIP] Executing: " << command << std::endl;

    int result = system(command.c_str());

    if (result != 0) {
        std::cout << "[UNZIP ERROR] unzip command failed with code: " << result << std::endl;
        if (error_msg) {
            *error_msg = "unzip command failed with exit code: " + std::to_string(result);
        }
        return false;
    }

    std::cout << "[UNZIP] Extraction completed successfully" << std::endl;
    return true;
}

#endif
