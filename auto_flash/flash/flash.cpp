#include <fstream>
#include <iomanip>
#include <sstream>
#include <ctime>

#include "flash/flash.h"

static const std::string SUCCESS_PATTERN = "The operation completed successfully";
static constexpr int SPINOR_TIMEOUT_SEC = 10 * 60;
static constexpr int HLOS_TIMEOUT_SEC   = 20 * 60;

static constexpr int MAX_RETRY = 1;
std::atomic<bool> flash_alive{true};

static constexpr int LOG_STALL_TIMEOUT_SEC = 10 * 60; // seconds

// SPINOR 燒完後裝置會重新列舉，太早開始 HLOS 會遇到
// "Did not receive Sahara hello packet" / Target returned NAK。
static constexpr int STAGE_GAP_SEC = 10;

// Helper function to send log to server
void send_comport_log(int comport, const std::string& log_msg);

// ======================
// run_emmcdl
// ======================
bool run_emmcdl(const std::string& folder,
                int comport,
                FlashType flash_type,
                std::atomic<bool>& flash_alive,
                Chipset chipset,
                StorageType storage,
                std::string* out_reason)
{
    auto set_reason = [&](const std::string& s) {
        if (out_reason) *out_reason = s;
    };
    // ===== 建立 log 檔名（COM + date + time）=====
    auto sys_now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(sys_now);
    std::tm tm{};
    localtime_s(&tm, &t);

    std::ostringstream log_name;
    log_name << folder << "\\flash_COM"
             << comport << "_"
             << std::put_time(&tm, "%Y%m%d_%H%M%S")
             << ".log";

    std::ofstream log_file(log_name.str(), std::ios::out | std::ios::app);

    auto write_log = [&](const std::string& s) {
        std::cout << s << std::flush;  // 🔧 即時 flush
        if (log_file.is_open()) {
            log_file << s;
            log_file.flush();
        }
        // Send log to server
        send_comport_log(comport, s);
    };

    write_log("[FLASH] Log file: " + log_name.str() + "\n");

    // ===== 組 command =====
    // 三種 chipset 的差別只有兩處：
    //   -f          loader     由 chipset 決定
    //   -Memoryname            SPINOR 階段固定 spinor，HLOS 階段由 storage 決定
    // XML 檔名三種 chipset 共用。
    std::string exe_path = folder + "\\emmcdl.exe";

    // Hamoa / Glymur 只支援 NVME，這裡再擋一次（不只依賴 UI / server）
    storage = clamp_storage(chipset, storage);

    const char* loader = loader_for(chipset);
    const char* xml    = (flash_type == FlashType::SPINOR) ? "rawprogram0.xml" : "WDFlash.xml";
    const char* mem    = (flash_type == FlashType::SPINOR) ? "spinor" : memoryname_for(storage);

    if (flash_type == FlashType::SPINOR)
        write_log("[FLASH] Flashing SPINOR...\n");
    else
        write_log("[FLASH] Flashing HLOS...\n");

    write_log(std::string("[FLASH] Chipset: ") + chipset_to_string(chipset)
              + ", Storage: " + storage_to_string(storage) + "\n");

    std::string command =
        "\"" + exe_path + "\" "
        "-p COM" + std::to_string(comport) +
        " -f " + loader +
        " -x " + xml +
        " -Memoryname " + mem;

    write_log("[FLASH] CMD: " + command + "\n");

    // ===== 建 pipe =====
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead, hWrite;
    CreatePipe(&hRead, &hWrite, &sa, 0);
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.hStdOutput = hWrite;
    si.hStdError  = hWrite;
    si.dwFlags   |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi{};

    // 環境區塊傳 nullptr = 繼承父行程環境。
    // 不可傳自訂區塊：CreateProcessA 會「整個取代」環境，emmcdl.exe 會失去
    // PATH / SystemRoot / TEMP。log 即時是靠下面的小 buffer + write_log 的 flush。
    BOOL ok = CreateProcessA(
        nullptr,
        command.data(),
        nullptr,
        nullptr,
        TRUE,
        0,
        nullptr,
        folder.c_str(),
        &si,
        &pi
    );

    CloseHandle(hWrite);

    if (!ok) {
        write_log("[FLASH ERROR] Failed to start emmcdl\n");
        set_reason("Failed to start emmcdl.exe");
        return false;
    }

    auto start_time    = std::chrono::steady_clock::now();
    auto last_log_time = start_time;
    flash_alive.store(true);

    bool success = false;

    char buffer[1024];  // 🔧 減小 buffer 大小，讓 log 更即時
    DWORD bytesRead;

    // SUCCESS_PATTERN 可能被切在兩次 ReadFile 之間，所以保留上一段的尾巴一起比對
    std::string carry;
    const size_t carry_keep = SUCCESS_PATTERN.size();

    // 讀 pipe 一次；有讀到資料回傳 true。偵測到成功字樣時設定 success。
    auto pump_pipe = [&]() -> bool {
        DWORD avail = 0;
        if (!PeekNamedPipe(hRead, nullptr, 0, nullptr, &avail, nullptr) || avail == 0)
            return false;

        DWORD toRead = (avail < sizeof(buffer) - 1) ? avail : (sizeof(buffer) - 1);
        if (!ReadFile(hRead, buffer, toRead, &bytesRead, nullptr) || bytesRead == 0)
            return false;

        buffer[bytesRead] = '\0';
        std::string log(buffer, bytesRead);
        write_log(log);

        // 跨 chunk 比對：前一段尾巴 + 這一段
        std::string probe = carry + log;
        if (probe.find(SUCCESS_PATTERN) != std::string::npos)
            success = true;

        carry = (probe.size() > carry_keep)
              ? probe.substr(probe.size() - carry_keep)
              : probe;

        return true;
    };

    // ===== main loop =====
    while (true)
    {
        // 🔧 使用非阻塞讀取，每 50ms 檢查一次
        if (pump_pipe())
        {
            last_log_time = std::chrono::steady_clock::now();

            if (success) {
                write_log("\n[FLASH] SUCCESS pattern detected\n");
                break;
            }
        }

        auto now = std::chrono::steady_clock::now();

        // ===== log stall timeout =====
        if (std::chrono::duration_cast<std::chrono::seconds>(
                now - last_log_time).count() > LOG_STALL_TIMEOUT_SEC)
        {
            write_log("\n[FLASH TIMEOUT] No log output for "
                      + std::to_string(LOG_STALL_TIMEOUT_SEC / 60) + " minutes\n");
            set_reason("No log output for " + std::to_string(LOG_STALL_TIMEOUT_SEC / 60) +
                       " minutes (emmcdl.exe stalled)");
            flash_alive.store(false);
            TerminateProcess(pi.hProcess, 1);
            break;
        }

        // ===== process exit =====
        DWORD exitCode;
        if (GetExitCodeProcess(pi.hProcess, &exitCode) &&
            exitCode != STILL_ACTIVE)
        {
            // emmcdl 收尾時會一次吐出大量 log 後立刻結束，pipe 裡通常還有殘留。
            // 這裡必須排空，否則 SUCCESS_PATTERN 會漏讀而誤判為 FAIL。
            while (pump_pipe()) {
                if (success) break;
            }

            if (success)
                write_log("\n[FLASH] SUCCESS pattern detected\n");
            else
                set_reason("emmcdl.exe exited (code " + std::to_string(exitCode) +
                           ") without success pattern");

            write_log("\n[FLASH] Process exited (code " + std::to_string(exitCode) + ")\n");
            break;
        }

        // ===== overall timeout =====
        if (flash_type == FlashType::SPINOR) {
            if (std::chrono::duration_cast<std::chrono::seconds>(
                    now - start_time).count() > SPINOR_TIMEOUT_SEC)
            {
                write_log("\n[FLASH ERROR] SPINOR timeout\n");
                set_reason("SPINOR timeout after " + std::to_string(SPINOR_TIMEOUT_SEC / 60) + " minutes");
                TerminateProcess(pi.hProcess, 1);
                break;
            }
        } else {
            if (std::chrono::duration_cast<std::chrono::seconds>(
                    now - start_time).count() > HLOS_TIMEOUT_SEC)
            {
                write_log("\n[FLASH ERROR] HLOS timeout\n");
                set_reason("HLOS timeout after " + std::to_string(HLOS_TIMEOUT_SEC / 60) + " minutes");
                TerminateProcess(pi.hProcess, 1);
                break;
            }
        }

        Sleep(50);
    }

    CloseHandle(hRead);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    write_log(std::string("[FLASH] Flash finished (") +
              (success ? "SUCCESS" : "FAIL") + ")\n");
    log_file.close();

    return success;
}

// ======================
// send_comport_log
// ======================
void send_comport_log(int comport, const std::string& log_msg)
{
    // Append log to comport_map
    std::lock_guard<std::mutex> lock(com_mutex);
    if (comport_map.find(comport) != comport_map.end()) {
        comport_map[comport].log += log_msg;
        
        // Limit log size to prevent memory issues (keep last 50KB)
        const size_t MAX_LOG_SIZE = 50 * 1024;
        if (comport_map[comport].log.size() > MAX_LOG_SIZE) {
            comport_map[comport].log = comport_map[comport].log.substr(
                comport_map[comport].log.size() - MAX_LOG_SIZE
            );
        }
    }
}

// ======================
// flash_image
// ======================
BOOL flash_image(const std::string& folder,
                 int comport,
                 FlashType flash_type,
                 Chipset chipset,
                 StorageType storage,
                 std::string* out_reason)
{
    for (int attempt = 1; attempt <= MAX_RETRY; ++attempt)
    {
        std::cout << "\nAttempt " << attempt << "/" << MAX_RETRY << "\n";

        if (run_emmcdl(folder, comport, flash_type, flash_alive, chipset, storage, out_reason))
            return 0;

        if (!flash_alive.load())
            std::cerr << "[FLASH FAILED] Log stalled > "
                      << (LOG_STALL_TIMEOUT_SEC / 60) << " minutes\n";
    }
    return 1;
}

// ======================
// flash_worker
// ======================
void flash_worker(const std::string& folder,
                  int comport,
                  Chipset chipset,
                  StorageType storage,
                  FlashStage flash_stage)
{
    storage = clamp_storage(chipset, storage);

    const bool do_spinor = stage_does_spinor(flash_stage);
    const bool do_hlos   = stage_does_hlos(flash_stage);

    std::cout << "[FLASH] Start flashing COM" << comport
              << " (chipset=" << chipset_to_string(chipset)
              << ", storage=" << storage_to_string(storage)
              << ", stage=" << stage_to_string(flash_stage)
              << ", loader=" << loader_for(chipset) << ")\n";

    auto mark_fail = [&](const char* which, const std::string& reason) {
        std::cerr << "Flash " << which << " FAILED: " << reason << "\n";
        std::lock_guard<std::mutex> lock(com_mutex);
        if (comport_map.find(comport) != comport_map.end()) {
            comport_map[comport].status = ComportStatus::FAIL;
            comport_map[comport].error_msg = std::string(which) + ": " + reason;
            std::cout << "[FLASH] COM" << comport << " status updated to [FAIL]\n";
        }
    };

    // 同時寫 console 和 server（Web UI 的 COM port log）
    auto stage_log = [&](const std::string& s) {
        std::cout << s << std::flush;
        send_comport_log(comport, s);
    };

    // ===== Flash SPINOR =====
    if (do_spinor) {
        std::cout << "[FLASH] Start flashing SPINOR COM" << comport << "\n";

        std::string reason;
        if (flash_image(folder, comport, FlashType::SPINOR, chipset, storage, &reason) != 0) {
            mark_fail("SPINOR", reason.empty() ? "unknown error" : reason);
            return;
        }

        std::cout << "Flash SPINOR SUCCESS\n";

        if (do_hlos) {
            // 等裝置重新列舉完成，逐秒倒數（console + Web UI 都看得到）
            stage_log("[FLASH] Waiting " + std::to_string(STAGE_GAP_SEC) +
                      "s for device to re-enumerate before HLOS...\n");

            for (int remain = STAGE_GAP_SEC; remain > 0; --remain) {
                stage_log("[FLASH] HLOS starts in " + std::to_string(remain) + "s...\n");
                Sleep(1000);
            }

            stage_log("[FLASH] Wait complete, starting HLOS\n");
        }
    } else {
        std::cout << "[FLASH] Skipping SPINOR (stage="
                  << stage_to_string(flash_stage) << ")\n";
    }

    // ===== Flash HLOS =====
    if (do_hlos) {
        std::cout << "[FLASH] Start flashing HLOS COM" << comport << "\n";

        std::string reason;
        if (flash_image(folder, comport, FlashType::HLOS, chipset, storage, &reason) != 0) {
            mark_fail("HLOS", reason.empty() ? "unknown error" : reason);
            return;
        }

        std::cout << "Flash HLOS SUCCESS\n";
    } else {
        std::cout << "[FLASH] Skipping HLOS (stage="
                  << stage_to_string(flash_stage) << ")\n";
    }

    // ===== All selected stages succeeded =====
    {
        std::lock_guard<std::mutex> lock(com_mutex);
        if (comport_map.find(comport) != comport_map.end()) {
            comport_map[comport].status = ComportStatus::SUCCESS;
            std::cout << "[FLASH] COM" << comport << " completed successfully [SUCCESS]\n";
        }
    }
}
