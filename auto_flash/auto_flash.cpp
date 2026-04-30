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

// bool auto_flash_status{false};
bool auto_flash{false};

std::atomic<bool> copy_cancel{false};
std::atomic<bool> copy_running{false};
std::thread copy_thread;


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

void copy_worker(std::string src, std::string dst)
{
    if (src.empty())
    {
        std::cout << "[COPY ERROR] src is empty\n";
        return;
    }
    if (dst.empty()){
        std::cout << "[COPY ERROR] dst is empty\n";
        return;
    }


    std::filesystem::path src_path(src);
    std::filesystem::path dst_path(dst);

    if (!std::filesystem::exists(src_path))
    {
        std::cout << "[COPY ERROR] src not exist: " << src << "\n";
        return;
    }

    // Installer Path 必須存在
    std::filesystem::create_directories(dst_path);

    try
    {
        // ===== CASE 1: 單一檔案 =====
        if (std::filesystem::is_regular_file(src_path))
        {
            if (copy_cancel)
            {
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
                    std::cout << "[COPY] cancelled\n";
                    return;
                }

                in.read(buf, sizeof(buf));
                out.write(buf, in.gcount());
            }
#endif
            return;
        }

        if (std::filesystem::is_directory(src_path))
        {
            for (const auto& entry :
                 std::filesystem::recursive_directory_iterator(src_path))
            {
                if (copy_cancel)
                {
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
                }
            }

            std::cout << "[COPY] folder done\n";
            return;
        }

        std::cout << "[COPY ERROR] unsupported src type\n";
    }
    catch (const std::exception& e)
    {
        std::cout << "[COPY ERROR] copy failed: "
                  << e.what() << "\n";
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
                            std::cout << "[EVENT] New COM detected: COM" << com << " [PENDING]\n";
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

                            com_cv.notify_one();
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
                        it = comport_map.erase(it);
                    } else {
                        ++it;
                    }
                }
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
            std::lock_guard<std::mutex> lock(com_mutex);
            for (const auto& [com, info] : comport_map)
            {
                if (!talker.send_msg(MSG_COMPORT, &info, sizeof(ComportInfo)))
                {
                    std::cout << "[Send failed] MSG_COMPORT COM" << com << "\n";
                }
                else
                {
                    const char* status_str = "UNKNOWN";
                    switch (info.status) {
                        case ComportStatus::PENDING:  status_str = "PENDING";  break;
                        case ComportStatus::FLASHING: status_str = "FLASHING"; break;
                        case ComportStatus::SUCCESS:  status_str = "SUCCESS";  break;
                        case ComportStatus::FAIL:     status_str = "FAIL";     break;
                    }
                    std::cout << "[Send success] MSG_COMPORT COM" << com << " [" << status_str << "]\n";
                }
            }
        }

        // ===== Send auto_flash status to server =====
        auto_flash = listener->auto_flash();
        if (!talker.send_msg(MSG_AUTO_FLASH_STATUS,
                            &auto_flash,
                            sizeof(auto_flash)))
        {
            std::cout << "[Send failed] MSG_AUTO_FLASH_STATUS\n";
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
                            copy_worker(download, installer);
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
            if (!std::filesystem::exists(listener.installer_path))
            {
                std::cout << "CAN'T FIND INSTALLER!!!  INSTALLER PATH: " << listener.installer_path << std::endl;
            }else{

                {
                    std::unique_lock<std::mutex> lock(com_mutex);
                    com_cv.wait(lock, [] {
                        return !new_com_queue.empty();
                    });

                    comport = new_com_queue.front();
                    new_com_queue.pop();
                }
                                // check validity and update status to FLASHING
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
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

    }

    
    


    
    return 1;
}