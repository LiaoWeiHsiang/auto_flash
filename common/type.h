#pragma once
#include <stdint.h>
#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <string>
#include <vector>

// 回傳 "[HH:MM:SS.mmm] "，方便對照 client / server 兩邊的 log 時序。
inline std::string ts()
{
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << '[' << std::put_time(&tm, "%H:%M:%S")
       << '.' << std::setfill('0') << std::setw(3) << ms.count() << "] ";
    return os.str();
}

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
    MSG_CLIENT_DOWNLOAD_PATH,   // Client reports current download path
    // NOTE: append new types at the END only. Inserting in the middle shifts
    // the wire numbering and breaks mixed old/new binaries.
    MSG_CHIPSET,         // Server -> Client: set chipset
    MSG_STORAGE,         // Server -> Client: set storage type
    MSG_CLIENT_CHIPSET,  // Client reports current chipset
    MSG_CLIENT_STORAGE,  // Client reports current storage type
    MSG_FLASH_STAGE,     // Server -> Client: which stage(s) to flash
    MSG_CLIENT_FLASH_STAGE  // Client reports current stage selection
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
    NOT_FOUND,          // installer_path does not exist
    FOUND,              // installer_path exists and emmcdl.exe is present
    COPYING,            // Currently copying / extracting
    COPY_COMPLETE,      // Copy completed
    COPY_FAILED,        // Copy / extraction failed
    PATH_NOT_A_FOLDER,  // installer_path exists but is not a directory
    WAITING_FOR_COPY,   // installer_path exists (folder) but emmcdl.exe not found, waiting for copy
    // NOTE: append new values at the END only (same reasoning as MsgType above).
    MISSING_FILES       // emmcdl.exe present, but files required by rawprogram0.xml/WDFlash.xml are missing
};

// ===== Flash target configuration =====
// The flash command is fully determined by these two settings:
//   - chipset chooses the loader passed to -f
//   - storage chooses the -Memoryname of the second (HLOS) stage
// The XML file names are shared by all chipsets.
enum class Chipset {
    KENAI,
    HAMOA,
    GLYMUR
};

enum class StorageType {
    NVME,
    UFS
};

// Only Kenai supports UFS; Hamoa / Glymur are NVME only.
inline bool chipset_supports_ufs(Chipset c)
{
    return c == Chipset::KENAI;
}

// Force an unsupported combination back to a valid one.
inline StorageType clamp_storage(Chipset c, StorageType s)
{
    return chipset_supports_ufs(c) ? s : StorageType::NVME;
}

inline const char* loader_for(Chipset c)
{
    return (c == Chipset::KENAI) ? "prog_firehose_ddr.elf"
                                 : "xbl_s_devprg_ns.melf";
}

inline const char* memoryname_for(StorageType s)
{
    return (s == StorageType::UFS) ? "ufs" : "nvme";
}

inline const char* chipset_to_string(Chipset c)
{
    switch (c) {
        case Chipset::HAMOA:  return "hamoa";
        case Chipset::GLYMUR: return "glymur";
        default:              return "kenai";
    }
}

// Unknown input falls back to the default rather than throwing.
inline Chipset chipset_from_string(const std::string& s)
{
    if (s == "hamoa")  return Chipset::HAMOA;
    if (s == "glymur") return Chipset::GLYMUR;
    return Chipset::KENAI;
}

inline const char* storage_to_string(StorageType s)
{
    return (s == StorageType::UFS) ? "ufs" : "nvme";
}

inline StorageType storage_from_string(const std::string& s)
{
    if (s == "ufs") return StorageType::UFS;
    return StorageType::NVME;
}

// 要燒哪些階段。BOTH 是預設（原本的行為）。
enum class FlashStage {
    BOTH,        // SPINOR -> HLOS
    SPINOR_ONLY,
    HLOS_ONLY
};

inline bool stage_does_spinor(FlashStage s)
{
    return s == FlashStage::BOTH || s == FlashStage::SPINOR_ONLY;
}

inline bool stage_does_hlos(FlashStage s)
{
    return s == FlashStage::BOTH || s == FlashStage::HLOS_ONLY;
}

inline const char* stage_to_string(FlashStage s)
{
    switch (s) {
        case FlashStage::SPINOR_ONLY: return "spinor_only";
        case FlashStage::HLOS_ONLY:   return "hlos_only";
        default:                      return "both";
    }
}

inline FlashStage stage_from_string(const std::string& s)
{
    if (s == "spinor_only") return FlashStage::SPINOR_ONLY;
    if (s == "hlos_only")   return FlashStage::HLOS_ONLY;
    return FlashStage::BOTH;
}

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
    std::string log;        // Flash log for this COM port
    std::string error_msg;  // Short human-readable failure reason (set when status == FAIL)
};

struct ClientInfo {
    std::string device_name;
    bool heartbeat = false;
    std::chrono::steady_clock::time_point last_seen;
    bool server_get_auto_flash_status;
    std::atomic<bool> auto_flash;
    std::string installer_path;
    std::string download_path;
    Chipset chipset = Chipset::KENAI;
    StorageType storage = StorageType::NVME;
    FlashStage flash_stage = FlashStage::BOTH;
    std::unordered_map<int, ComportInfo> comport_list;
    FileInfo file_info;  // File copy status
};