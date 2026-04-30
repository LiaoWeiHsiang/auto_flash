#pragma once
#include <stdint.h>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <string>
#include <vector>

enum MsgType {
    MSG_HEART_BEAT,
    MSG_AUTO_FLASH_STATUS,
    MSG_COMPORT,
    MSG_PROGRESS,
    MSG_LOG,
    MSG_SET_AUTO_FLASH,
    MSG_INSTALLER_PATH,
    MSG_DOWNLOAD_PATH,
    MSG_COMPORT_LOG,     // COM port specific log
    MSG_FILE_STATUS,     // File copy status
    MSG_CLIENT_INSTALLER_PATH,  // Client reports current installer path
    MSG_CLIENT_DOWNLOAD_PATH    // Client reports current download path
};

struct Packet {
    uint8_t type;
    uint32_t length;
    char data[];
};

enum class FlashType {
    NONE,
    SPINOR,
    HLOS
};

enum class ComportStatus{
    PENDING,
    FLASHING,
    FAIL,
    SUCCESS,
    REMOVED  // COM port has been removed
};

enum class FileStatus {
    NOT_FOUND,      // Installer not found
    FOUND,          // Installer found
    COPYING,        // Currently copying
    COPY_COMPLETE,  // Copy completed
    COPY_FAILED     // Copy failed
};

struct FileInfo {
    FileStatus status = FileStatus::NOT_FOUND;
    uint64_t total_bytes = 0;      // Total file size
    uint64_t copied_bytes = 0;     // Copied bytes
    double progress = 0.0;         // Progress percentage (0-100)
    double speed_mbps = 0.0;       // Copy speed in MB/s
    int eta_seconds = 0;           // Estimated time remaining
    std::string error_msg;         // Error message if failed
};

struct ComportInfo {
    int number;
    ComportStatus status = ComportStatus::PENDING;
    std::string log;  // Flash log for this COM port
};

struct ClientInfo {
    std::string device_name;
    bool heartbeat = false;
    std::chrono::steady_clock::time_point last_seen;
    bool server_get_auto_flash_status;
    std::atomic<bool> auto_flash;
    std::string installer_path;
    std::string download_path;
    std::unordered_map<int, ComportInfo> comport_list;
    FileInfo file_info;  // File copy status
};