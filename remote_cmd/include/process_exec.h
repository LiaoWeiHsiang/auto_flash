#pragma once
#include <functional>
#include <string>

// Runs `command` through the platform shell (cmd.exe /C on Windows,
// /bin/sh -c on Linux), invoking on_output for each chunk of merged
// stdout+stderr as soon as it is available (so callers can stream it
// out live). Returns the process exit code, or -1 if the process could
// not be started at all. A process killed by a signal on POSIX reports
// 128 + signal number, matching shell convention.
int run_command(const std::string& command,
                 const std::function<void(const char* data, size_t len)>& on_output);
