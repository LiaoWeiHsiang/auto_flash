
#include "flash/flash.h"

static const std::string SUCCESS_PATTERN = "The operation completed successfully";
static constexpr int SPINOR_TIMEOUT_SEC = 10 * 60;
static constexpr int HLOS_TIMEOUT_SEC = 20 * 60;

static constexpr int MAX_RETRY = 3;

bool run_emmcdl(const std::string& folder, int comport, FlashType flash_type)
{
    // std::string command =
    //     "emmcdl.exe -p COM" + std::to_string(comport) +
    //     " -f xbl_s_devprg_ns.melf -x rawprogram0.xml -Memoryname spinor";
    std::string exe_path = folder + "\\emmcdl.exe";
    std::string command;
    if (flash_type == FlashType::SPINOR) {
        std::cout << "Flashing SPINOR...\n";
        
        command =
        "\"" + exe_path + "\" "
        "-p COM" + std::to_string(comport) +
        " -f xbl_s_devprg_ns.melf"
        " -x rawprogram0.xml"
        " -Memoryname spinor";

        // command =
        // "\"C:\\workspace\\Glymur\\r4900\\Installer\\emmcdl.exe\" "
        // "-p COM" + std::to_string(comport) +
        // " -f xbl_s_devprg_ns.melf"
        // " -x rawprogram0.xml"
        // " -Memoryname spinor";
     } else {
        std::cout << "Flashing HLOS (Windows)...\n";

        command =
        "\"" + exe_path + "\" "
        "-p COM" + std::to_string(comport) +
        " -f xbl_s_devprg_ns.melf"
        " -x WDFlash.xml"
        " -Memoryname nvme";

        // command =
        // "\"C:\\workspace\\Glymur\\r4900\\Installer\\emmcdl.exe\" "
        // "-p COM" + std::to_string(comport) +
        // " -f xbl_s_devprg_ns.melf"
        // " -x WDFlash.xml"
        // " -Memoryname nvme";
     }
    

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead, hWrite;
    CreatePipe(&hRead, &hWrite, &sa, 0);
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi{};

    std::cout << "Create Command Process: " << command << "\n";

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
    std::cout << "Create Command Process completed: " << command << "\n";

    CloseHandle(hWrite);

    if (!ok) {
        std::cerr << " Failed to start emmcdl\n";
        return false;
    }

    auto start = std::chrono::steady_clock::now();
    bool success = false;

    char buffer[4096];
    DWORD bytesRead;

    while (true)
    {
        if (PeekNamedPipe(hRead, nullptr, 0, nullptr, &bytesRead, nullptr) && bytesRead > 0)
        {
            ReadFile(hRead, buffer, sizeof(buffer) - 1, &bytesRead, nullptr);
            buffer[bytesRead] = '\0';

            std::string log(buffer);
            std::cout << log;

            if (log.find(SUCCESS_PATTERN) != std::string::npos) {
                std::cout << "\n Success pattern detected\n";
                success = true;
                break;
            }
        }

        DWORD exitCode;
        if (GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode != STILL_ACTIVE)
            break;

        auto now = std::chrono::steady_clock::now();
        if(flash_type == FlashType::SPINOR){
            if (std::chrono::duration_cast<std::chrono::seconds>(now - start).count() > SPINOR_TIMEOUT_SEC)
            {
                std::cerr << "\n Timeout, killing process\n";
                TerminateProcess(pi.hProcess, 1);
                break;
            }
        }
        else if(flash_type == FlashType::HLOS){
            if (std::chrono::duration_cast<std::chrono::seconds>(now - start).count() > HLOS_TIMEOUT_SEC)
            {
                std::cerr << "\n Timeout, killing process\n";
                TerminateProcess(pi.hProcess, 1);
                break;
            }
        }

        Sleep(50);
    }

    CloseHandle(hRead);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return success;
}



BOOL flash_image(const std::string& folder, int comport, FlashType flash_type)
{

    for (int attempt = 1; attempt <= MAX_RETRY; ++attempt){
        std::cout << "\nAttempt " << attempt << "/" << MAX_RETRY << "\n";

        if (run_emmcdl(folder, comport, flash_type))
        {
            return 0;
        }

        std::cout << "\nRetry...\n";
    }
    return 1;

}


void flash_worker(const std::string& folder, int comport)
{
    std::cout << "[FLASH] Start flashing spinor COM" << comport << "\n";

    if(flash_image(folder, comport, FlashType::SPINOR) != 0){
            std::cerr << "\nFlash spinor FAILED\n";
            return;
        }else{
            std::cout << "\nFlash spinor SUCCESS\n";
        }

    Sleep(1 * 1000);  // 1s

    std::cout << "[FLASH] Start flashing hlos COM" << comport << "\n";
    if(flash_image(folder, comport, FlashType::HLOS) != 0){
        std::cerr << "Flash HLOS FAILED\n";
        return;
    }else{
        std::cout << "Flash HLOS SUCCESS\n";
    }


    // Fail retry
    /*
    if (failed) {
        std::lock_guard<std::mutex> lock(comport_mutex);
        seen_comports.erase(comport)
    }
    */
}