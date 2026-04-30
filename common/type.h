#pragma once
#include <stdint.h>
#include <atomic>
#include <chrono>
#include <unordered_map>

enum MsgType {
    MSG_HEART_BEAT,
    MSG_AUTO_FLASH_STATUS,
    MSG_COMPORT,
    MSG_PROGRESS,
    MSG_LOG,
    MSG_SET_AUTO_FLASH,
    MSG_INSTALLER_PATH,
    MSG_DOWNLOAD_PATH
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
    SUCCESS
};

struct ComportInfo {
    int number;
    ComportStatus status = ComportStatus::PENDING;
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
};