#include <windows.h>
#include <iostream>
#include <string>
#include <chrono>
#include <atomic>
#include <filesystem>
#include "common/type.h"
#include "comport/comport.h"
#include "flash/flash.h"
#include "utils/talker/talker.h"
#include "utils/listener/listener.h"

#include <queue>
#include <set>
#include <unordered_map>
#include <mutex>
#include <condition_variable>

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
std::atomic<bool> copy_running{false};
std::thread copy_thread;

// Helper function to send file status
void send_file_status(const std::string& ip, const FileInfo& info)
{
    Talker talker(ip, 9000);
    
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
    
    talker.send_msg(MSG_FILE_STATUS, buffer.data(), buffer.size());
}


#ifdef _WIN32
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
    std::atomic<bool>* cancel =
        static_cast<std::atomic<bool>*>(lpData);

    if (cancel && cancel->load())
        return PROGRESS_CANCEL; 

    return PROGRESS_CONTINUE;
}

bool copy_file_win32(const std::wstring& src,
                     const std::wstring& dst,
                     std::atomic<bool>* cancel)
{
    BOOL ok = CopyFileExW(
        src.c_str(),
        dst.c_str(),
        CopyProgressRoutine,  
        cancel,             
        nullptr,
        COPY_FILE_RESTARTABLE
    );

    if (!ok)
    {
        DWORD err = GetLastError();
        if (err == ERROR_REQUEST_ABORTED)
            std::cout << "[COPY] cancelled\n";
        else
            std::cout << "[COPY ERROR] CopyFileEx failed: " << err << "\n";

        return false;
    }

    return true;
}
#endif

void copy_worker(std::string src, std::string dst, std::string ip)
{
    // Initialize file status
    {
        std::lock_guard<std::mutex> lock(file_info_mutex);
        global_file_info.status = FileStatus::COPYING;
        global_file_info.progress = 0.0;
        global_file_info.copied_bytes = 0;
        global_file_info.total_bytes = 0;
        global_file_info.speed_mbps = 0.0;
        global_file_info.eta_seconds = 0;
        global_file_info.error_msg = "";
    }
    send_file_status(ip, global_file_info);
    
    if (src.empty())
    {
        std::lock_guard<std::mutex> lock(file_info_mutex);
        global_file_info.status = FileStatus::COPY_FAILED;
        global_file_info.error_msg = "Source path is empty";
        send_file_status(ip, global_file_info);
        std::cout << "[COPY ERROR] src is empty\n";
        return;
    }
    if (dst.empty()){
        std::lock_guard<std::mutex> lock(file_info_mutex);
        global_file_info.status = FileStatus::COPY_FAILED;
        global_file_info.error_msg = "Destination path is empty";
        send_file_status(ip, global_file_info);
        std::cout << "[COPY ERROR] dst is empty\n";
        return;
    }


    std::filesystem::path src_path(src);
    std::filesystem::path dst_path(dst);

    if (!std::filesystem::exists(src_path))
    {
        std::lock_guard<std::mutex> lock(file_info_mutex);
        global_file_info.status = FileStatus::COPY_FAILED;
        global_file_info.error_msg = "Source path not exist: " + src;
        send_file_status(ip, global_file_info);
        std::cout << "[COPY ERROR] src not exist: " << src << "\n";
        return;
    }

    // Installer Path 必須存在
    std::filesystem::create_directories(dst_path);

    try
    {
        // Calculate total size
        uint64_t total_size = 0;
        int total_files = 0;
        
        if (std::filesystem::is_regular_file(src_path)) {
            total_size = std::filesystem::file_size(src_path);
            total_files = 1;
        } else if (std::filesystem::is_directory(src_path)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(src_path)) {
                if (entry.is_regular_file()) {
                    total_size += entry.file_size();
                    total_files++;
                }
            }
        }
        
        {
            std::lock_guard<std::mutex> lock(file_info_mutex);
            global_file_info.total_bytes = total_size;
        }
        send_file_status(ip, global_file_info);
        
        auto start_time = std::chrono::steady_clock::now();
        uint64_t copied_so_far = 0;
        int file_count = 0;
        
        // ===== CASE 1: 單一檔案 =====
        if (std::filesystem::is_regular_file(src_path))
        {
            if (copy_cancel)
            {
                std::lock_guard<std::mutex> lock(file_info_mutex);
                global_file_info.status = FileStatus::COPY_FAILED;
                global_file_info.error_msg = "Copy cancelled";
                send_file_status(ip, global_file_info);
                std::cout << "[COPY] cancelled before start\n";
                return;
            }

            std::filesystem::path target =
                dst_path / src_path.filename();

#ifdef _WIN32
            copy_file_win32(
                src_path.wstring(),
                target.wstring(),
                &copy_cancel
            );
#else
            // Linux / POSIX
            std::ifstream in(src, std::ios::binary);
            std::ofstream out(target, std::ios::binary);

            char buf[1024 * 64];
            while (in && out)
            {
                if (copy_cancel)
                {
                    std::lock_guard<std::mutex> lock(file_info_mutex);
                    global_file_info.status = FileStatus::COPY_FAILED;
                    global_file_info.error_msg = "Copy cancelled";
                    send_file_status(ip, global_file_info);
                    std::cout << "[COPY] cancelled\n";
                    return;
                }

                in.read(buf, sizeof(buf));
                out.write(buf, in.gcount());
            }
#endif
            
            // Update progress
            copied_so_far = total_size;
            {
                std::lock_guard<std::mutex> lock(file_info_mutex);
                global_file_info.copied_bytes = copied_so_far;
                global_file_info.progress = 100.0;
                global_file_info.status = FileStatus::COPY_COMPLETE;
            }
            send_file_status(ip, global_file_info);
            return;
        }

        if (std::filesystem::is_directory(src_path))
        {
            for (const auto& entry :
                 std::filesystem::recursive_directory_iterator(src_path))
            {
                if (copy_cancel)
                {
                    std::lock_guard<std::mutex> lock(file_info_mutex);
                    global_file_info.status = FileStatus::COPY_FAILED;
                    global_file_info.error_msg = "Copy cancelled";
                    send_file_status(ip, global_file_info);
                    std::cout << "[COPY] cancelled\n";
                    return;
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

#ifdef _WIN32
                    copy_file_win32(
                        entry.path().wstring(),
                        target.wstring(),
                        &copy_cancel
                    );
#else
                    std::ifstream in(entry.path(), std::ios::binary);
                    std::ofstream out(target, std::ios::binary);

                    char buf[1024 * 64];
                    while (in && out)
                    {
                        if (copy_cancel)
                            return;

                        in.read(buf, sizeof(buf));
                        out.write(buf, in.gcount());
                    }
#endif
                    
                    // Update progress
                    copied_so_far += file_size;
                    file_count++;
                    
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
                    
                    {
                        std::lock_guard<std::mutex> lock(file_info_mutex);
                        global_file_info.copied_bytes = copied_so_far;
                        global_file_info.progress = (double)copied_so_far / total_size * 100.0;
                        
                        if (elapsed > 0) {
                            double mb_copied = copied_so_far / (1024.0 * 1024.0);
                            global_file_info.speed_mbps = mb_copied / elapsed;
                            
                            uint64_t remaining = total_size - copied_so_far;
                            if (copied_so_far > 0) {
                                global_file_info.eta_seconds = (int)(remaining * elapsed / copied_so_far);
                            }
                        }
                    }
                    
                    // Send status every 10 files
                    if (file_count % 10 == 0) {
                        send_file_status(ip, global_file_info);
                    }
                }
            }

            std::cout << "[COPY] folder done\n";
            
            // Mark as complete
            {
                std::lock_guard<std::mutex> lock(file_info_mutex);
                global_file_info.status = FileStatus::COPY_COMPLETE;
                global_file_info.progress = 100.0;
                global_file_info.copied_bytes = total_size;
            }
            send_file_status(ip, global_file_info);
            return;
        }

        std::cout << "[COPY ERROR] unsupported src type\n";
        {
            std::lock_guard<std::mutex> lock(file_info_mutex);
            global_file_info.status = FileStatus::COPY_FAILED;
            global_file_info.error_msg = "Unsupported source type";
        }
        send_file_status(ip, global_file_info);
    }
    catch (const std::exception& e)
    {
        std::cout << "[COPY ERROR] copy failed: "
                  << e.what() << "\n";
        
        std::lock_guard<std::mutex> lock(file_info_mutex);
        global_file_info.status = FileStatus::COPY_FAILED;
        global_file_info.error_msg = e.what();
        send_file_status(ip, global_file_info);
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
    Talker talker(ip, 9000);
    auto last_print_time = std::chrono::steady_clock::now();
    while (true)
    {
        std::vector<int> current_coms;
        std::set<int> current_set;

                // ===== Scan current COMs =====
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

                        std::cout << "[EVENT] New COM detected: COM" << com << " [PENDING]\n";

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
                            std::cout << "[QUEUE ] Added COM" << com << " to queue";
                            if (!auto_flash) {
                                std::cout << " (waiting for auto_flash to be enabled)";
                            }
                            std::cout << "\n";
                            
                            std::cout << "[QUEUE ] new_com_queue: ";
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
                            std::cout << "[SKIP ] COM" << com
                                    << " already in new_com_queue\n";
                        }
                    }
                }

                                                // ===== Removed COM detection =====
                for (auto it = comport_map.begin(); it != comport_map.end(); )
                {
                    if (current_set.find(it->first) == current_set.end()) {
                        std::cout << "[EVENT] COM removed: COM" << it->first << "\n";
                        
                        // Mark as REMOVED before erasing
                        it->second.status = ComportStatus::REMOVED;
                        removed_ports.push_back(it->second);
                        
                        it = comport_map.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            
            // 🔧 在鎖外發送 REMOVED 狀態
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
                
                Talker talker_temp(ip, 9000);
                talker_temp.send_msg(MSG_COMPORT, buffer.data(), buffer.size());
            }
        }


        
        // print logs
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(
                now - last_print_time).count() >= 60)
        {
            std::lock_guard<std::mutex> lock(com_mutex);

            // ===== print current Scan =====
            std::cout << "[SCAN ] get_all_9008_comports: ";
            if (current_set.empty()) {
                std::cout << "(none)";
            } else {
                for (int com : current_set) {
                    std::cout << "COM" << com << " ";
                }
            }
            std::cout << std::endl;

            // ===== Print current comport_map =====
            std::cout << "[STATE] comport_map:             ";
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

        if (!talker.send_msg(MSG_HEART_BEAT,
                            device_name.c_str(),
                            device_name.size()))
        {
            std::cout << "[Send failed] MSG_HEART_BEAT\n";
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
                
                if (!talker.send_msg(MSG_COMPORT, buffer.data(), buffer.size()))
                {
                    std::cout << "[Send failed] MSG_COMPORT COM" << com << "\n";
                }
            }
        }

                                // ===== Send auto_flash status to server =====
        bool current_auto_flash = listener->auto_flash();
        
        // Detect auto_flash state change from false to true
        if (!auto_flash && current_auto_flash) {
            std::cout << "[AUTO_FLASH] Enabled! Notifying main loop to process queue...\n";
            com_cv.notify_one();  // Wake up main loop
        }
        
        auto_flash = current_auto_flash;
        
        if (!talker.send_msg(MSG_AUTO_FLASH_STATUS,
                            &auto_flash,
                            sizeof(auto_flash)))
        {
            std::cout << "[Send failed] MSG_AUTO_FLASH_STATUS\n";
        }
        
        // ===== Send current client paths to server =====
        std::string current_installer = listener->installer_path;
        std::string current_download = listener->download_path;
        
        if (!talker.send_msg(MSG_CLIENT_INSTALLER_PATH,
                            current_installer.c_str(),
                            current_installer.size()))
        {
            std::cout << "[Send failed] MSG_CLIENT_INSTALLER_PATH\n";
        }
        
        if (!talker.send_msg(MSG_CLIENT_DOWNLOAD_PATH,
                            current_download.c_str(),
                            current_download.size()))
        {
            std::cout << "[Send failed] MSG_CLIENT_DOWNLOAD_PATH\n";
        }
        
                // ===== Check installer status and send file status =====
        {
            std::lock_guard<std::mutex> lock(file_info_mutex);
            
            if (std::filesystem::exists(listener->installer_path)) {
                if (global_file_info.status != FileStatus::COPYING && 
                    global_file_info.status != FileStatus::COPY_COMPLETE) {
                    global_file_info.status = FileStatus::FOUND;
                    
                    // Calculate folder size
                    try {
                        uint64_t total_size = 0;
                        std::filesystem::path installer_path(listener->installer_path);
                        
                        if (std::filesystem::is_regular_file(installer_path)) {
                            total_size = std::filesystem::file_size(installer_path);
                        } else if (std::filesystem::is_directory(installer_path)) {
                            for (const auto& entry : std::filesystem::recursive_directory_iterator(installer_path)) {
                                if (entry.is_regular_file()) {
                                    total_size += entry.file_size();
                                }
                            }
                        }
                        
                        global_file_info.total_bytes = total_size;
                        global_file_info.copied_bytes = total_size;  // Already complete
                        global_file_info.progress = 100.0;
                    } catch (const std::exception& e) {
                        std::cout << "[FILE_INFO] Failed to calculate size: " << e.what() << "\n";
                    }
                }
            } else {
                if (global_file_info.status != FileStatus::COPYING) {
                    global_file_info.status = FileStatus::NOT_FOUND;
                    global_file_info.total_bytes = 0;
                    global_file_info.copied_bytes = 0;
                    global_file_info.progress = 0.0;
                }
            }
            
            send_file_status(ip, global_file_info);
        }
        

        // ===== Check if need to copy installer =====
        if (auto_flash )
        {
            if(copy_conuter%5==0){
                std::string installer = listener->installer_path;
                std::string download  = listener->download_path;
                copy_conuter = 0;
                if (!std::filesystem::exists(installer))
                {
                    if (!copy_running)
                    {
                        copy_cancel = false;
                        copy_running = true;

                                                copy_thread = std::thread([=]() {
                            copy_worker(download, installer, ip);  // Pass ip parameter
                            copy_running = false;
                        });

                        copy_thread.detach();
                    }
                }else{

                    // bool complete = is_folder_complete(download, installer);
                    // if (!complete){
                    //     std::cout << "installer path: " << installer << " maybe incomplete!!!" << std::endl;

                    // }
                }
            }
        }
        else
        {
            copy_cancel = true;
            copy_conuter = 0;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
    }
}

int main(int argc, char* argv[])
{

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


    std::thread com_monitor_thread(com_monitor,ip, &listener);
    com_monitor_thread.detach();    

    

            while (true)
    {
        int comport = -1;

        if (auto_flash)
        {
            // Check if installer exists BEFORE processing queue
            if (!std::filesystem::exists(listener.installer_path))
            {
                std::cout << "[WAITING] Installer not found: " << listener.installer_path << std::endl;
                std::cout << "[WAITING] Queue has " << new_com_queue.size() << " COM port(s) waiting for installer...\n";
                
                // Don't process queue, just wait
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                continue;
            }

            // Installer exists, process queue
            {
                std::unique_lock<std::mutex> lock(com_mutex);
                com_cv.wait(lock, [] {
                    return !new_com_queue.empty();
                });

                // Check auto_flash again after waking up
                if (!auto_flash) {
                    std::cout << "[MAIN] Auto flash disabled, clearing queue\n";
                    // Clear the queue
                    while (!new_com_queue.empty()) {
                        int skipped = new_com_queue.front();
                        new_com_queue.pop();
                        std::cout << "[SKIP] COM" << skipped << " (auto flash disabled)\n";
                    }
                    continue;
                }

                // Double check installer exists before popping from queue
                if (!std::filesystem::exists(listener.installer_path)) {
                    std::cout << "[WAITING] Installer disappeared, keeping COM ports in queue\n";
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
                    std::cout << "[MAIN] Start flashing COM" << comport << " [FLASHING]\n";
                    
                    std::thread flash_thread(
                        flash_worker,
                        listener.installer_path,
                        comport
                    );
                    flash_thread.detach();
                }
                else
                {
                    std::cout << "[SKIP] COM not in comport_map: " << comport << "\n";
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