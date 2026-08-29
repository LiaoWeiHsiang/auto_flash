#include "process_exec.h"

#ifdef _WIN32
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0601
    #endif
    #include <windows.h>
#else
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <unistd.h>
    #include <cstring>
#endif

#ifdef _WIN32

// ======================
// Windows: CreateProcess + pipe, same pattern as
// auto_flash/flash/flash.cpp::run_emmcdl and utils/zip/unzip.cpp.
// ======================
int run_command(const std::string& command,
                 const std::function<void(const char* data, size_t len)>& on_output)
{
    std::string cmdline = "cmd.exe /C \"" + command + "\"";

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead, hWrite;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0))
        return -1;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};

    // Environment nullptr = inherit parent's (PATH / SystemRoot / TEMP etc).
    BOOL ok = CreateProcessA(
        nullptr,
        cmdline.data(),
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

    if (!ok) {
        CloseHandle(hRead);
        return -1;
    }

    char buffer[4096];
    DWORD bytesRead;

    auto pump_pipe = [&]() -> bool {
        DWORD avail = 0;
        if (!PeekNamedPipe(hRead, nullptr, 0, nullptr, &avail, nullptr) || avail == 0)
            return false;

        DWORD toRead = (avail < sizeof(buffer)) ? avail : sizeof(buffer);
        if (!ReadFile(hRead, buffer, toRead, &bytesRead, nullptr) || bytesRead == 0)
            return false;

        on_output(buffer, bytesRead);
        return true;
    };

    while (true) {
        if (pump_pipe())
            continue;

        DWORD exitCode;
        if (GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
            // Drain whatever is left in the pipe before declaring done.
            while (pump_pipe()) {}

            CloseHandle(hRead);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return static_cast<int>(exitCode);
        }

        Sleep(20);
    }
}

#else

// ======================
// Linux/POSIX: fork + exec + pipe, stdout and stderr both redirected to
// the same pipe so the host sees one merged, ordered stream.
// ======================
int run_command(const std::string& command,
                 const std::function<void(const char* data, size_t len)>& on_output)
{
    int fds[2];
    if (pipe(fds) != 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }

    if (pid == 0) {
        // Child.
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        execl("/bin/sh", "sh", "-c", command.c_str(), (char*)nullptr);
        _exit(127);  // execl failed
    }

    // Parent.
    close(fds[1]);

    char buffer[4096];
    ssize_t n;
    while ((n = read(fds[0], buffer, sizeof(buffer))) > 0) {
        on_output(buffer, static_cast<size_t>(n));
    }
    close(fds[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return -1;
}

#endif
