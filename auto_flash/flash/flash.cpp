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

// ======================
// run_emmcdl
// ======================
bool run_emmcdl(const std::string& folder,
                int comport,
                FlashType flash_type,
                std::atomic<bool>& flash_alive)
{
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
        std::cout << s;
        if (log_file.is_open()) {
            log_file << s;
            log_file.flush();
        }
    };

    write_log("[FLASH] Log file: " + log_name.str() + "\n");

    // ===== 組 command =====
    std::string exe_path = folder + "\\emmcdl.exe";
    std::string command;

    if (flash_type == FlashType::SPINOR) {
        write_log("[FLASH] Flashing SPINOR...\n");
        command =
            "\"" + exe_path + "\" "
            "-p COM" + std::to_string(comport) +
            " -f xbl_s_devprg_ns.melf"
            " -x rawprogram0.xml"
            " -Memoryname spinor";
    } else {
        write_log("[FLASH] Flashing HLOS...\n");
        command =
            "\"" + exe_path + "\" "
            "-p COM" + std::to_string(comport) +
            " -f xbl_s_devprg_ns.melf"
            " -x WDFlash.xml"
            " -Memoryname nvme";
    }

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
        return false;
    }

    auto start_time    = std::chrono::steady_clock::now();
    auto last_log_time = start_time;
    flash_alive.store(true);

    bool success = false;

    char buffer[4096];
    DWORD bytesRead;

    // ===== main loop =====
    while (true)
    {
        if (PeekNamedPipe(hRead, nullptr, 0, nullptr, &bytesRead, nullptr)
            && bytesRead > 0)
        {
            ReadFile(hRead, buffer, sizeof(buffer) - 1, &bytesRead, nullptr);
            buffer[bytesRead] = '\0';

            std::string log(buffer);
            write_log(log);

            last_log_time = std::chrono::steady_clock::now();

            if (log.find(SUCCESS_PATTERN) != std::string::npos) {
                write_log("\n[FLASH] SUCCESS pattern detected\n");
                success = true;
                break;
            }
        }

        auto now = std::chrono::steady_clock::now();

        // ===== log stall timeout =====
        if (std::chrono::duration_cast<std::chrono::seconds>(
                now - last_log_time).count() > LOG_STALL_TIMEOUT_SEC)
        {
            write_log("\n[FLASH TIMEOUT] No log output for 3 minutes\n");
            flash_alive.store(false);
            TerminateProcess(pi.hProcess, 1);
            break;
        }

        // ===== process exit =====
        DWORD exitCode;
        if (GetExitCodeProcess(pi.hProcess, &exitCode) &&
            exitCode != STILL_ACTIVE)
        {
            write_log("\n[FLASH] Process exited\n");
            break;
        }

        // ===== overall timeout =====
        if (flash_type == FlashType::SPINOR) {
            if (std::chrono::duration_cast<std::chrono::seconds>(
                    now - start_time).count() > SPINOR_TIMEOUT_SEC)
            {
                write_log("\n[FLASH ERROR] SPINOR timeout\n");
                TerminateProcess(pi.hProcess, 1);
                break;
            }
        } else {
            if (std::chrono::duration_cast<std::chrono::seconds>(
                    now - start_time).count() > HLOS_TIMEOUT_SEC)
            {
                write_log("\n[FLASH ERROR] HLOS timeout\n");
                TerminateProcess(pi.hProcess, 1);
                break;
            }
        }

        Sleep(50);
    }

    CloseHandle(hRead);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    write_log("[FLASH] Flash finished\n");
    log_file.close();

    return success;
}

// ======================
// flash_image
// ======================
BOOL flash_image(const std::string& folder, int comport, FlashType flash_type)
{
    for (int attempt = 1; attempt <= MAX_RETRY; ++attempt)
    {
        std::cout << "\nAttempt " << attempt << "/" << MAX_RETRY << "\n";

        if (run_emmcdl(folder, comport, flash_type, flash_alive))
            return 0;

        if (!flash_alive.load())
            std::cerr << "[FLASH FAILED] Log stalled > 3 minutes\n";
    }
    return 1;
}

// ======================
// flash_worker
// ======================
void flash_worker(const std::string& folder, int comport)
{
    std::cout << "[FLASH] Start flashing SPINOR COM" << comport << "\n";

    bool spinor_success = false;
    bool hlos_success = false;

    // ===== Flash SPINOR =====
    if (flash_image(folder, comport, FlashType::SPINOR) != 0) {
        std::cerr << "Flash SPINOR FAILED\n";
        
        // Update status to FAIL
        {
            std::lock_guard<std::mutex> lock(com_mutex);
            if (comport_map.find(comport) != comport_map.end()) {
                comport_map[comport].status = ComportStatus::FAIL;
                std::cout << "[FLASH] COM" << comport << " status updated to [FAIL]\n";
            }
        }
        return;
    }

    std::cout << "Flash SPINOR SUCCESS\n";
    spinor_success = true;
    Sleep(1000);

    // ===== Flash HLOS =====
    std::cout << "[FLASH] Start flashing HLOS COM" << comport << "\n";

    if (flash_image(folder, comport, FlashType::HLOS) != 0) {
        std::cerr << "Flash HLOS FAILED\n";
        
        // Update status to FAIL
        {
            std::lock_guard<std::mutex> lock(com_mutex);
            if (comport_map.find(comport) != comport_map.end()) {
                comport_map[comport].status = ComportStatus::FAIL;
                std::cout << "[FLASH] COM" << comport << " status updated to [FAIL]\n";
            }
        }
        return;
    }

    std::cout << "Flash HLOS SUCCESS\n";
    hlos_success = true;

    // ===== Both succeeded =====
    if (spinor_success && hlos_success) {
        std::lock_guard<std::mutex> lock(com_mutex);
        if (comport_map.find(comport) != comport_map.end()) {
            comport_map[comport].status = ComportStatus::SUCCESS;
            std::cout << "[FLASH] COM" << comport << " completed successfully [SUCCESS]\n";
        }
    }
}
