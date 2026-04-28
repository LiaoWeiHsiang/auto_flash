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
#include <mutex>
#include <condition_variable>

std::queue<int> new_com_queue;
std::set<int> seen_comports;
std::vector<int> current_coms;

std::mutex com_mutex;
std::condition_variable com_cv;

// bool auto_flash_status{false};
bool auto_flash{false};

std::atomic<bool> copy_cancel{false};
std::atomic<bool> copy_running{false};
std::thread copy_thread;
void copy_worker(std::string src, std::string dst)
{
    // std::cout << "[COPY] start\n";
    // std::cout << "[COPY] src = [" << src << "]\n";
    // std::cout << "[COPY] dst = [" << dst << "]\n";

    // ===== empty path =====
    if (src.empty() || dst.empty())
    {
        std::cout << "[COPY ERROR] src or dst is empty\n";
        return;
    }

    // ===== src must exist =====
    if (!std::filesystem::exists(src))
    {
        std::cout << "[COPY ERROR] src not exist: " << src << "\n";
        return;
    }

    try
    {
        std::filesystem::create_directories(dst);
    }
    catch (const std::exception& e)
    {
        std::cout << "[COPY ERROR] create_directories failed: " << e.what() << "\n";
        return;
    }

    try
    {
        for (auto& entry : std::filesystem::recursive_directory_iterator(src))
        {
            if (copy_cancel)
            {
                std::cout << "[COPY] cancelled\n";
                return;
            }

            auto rel = std::filesystem::relative(entry.path(), src);
            auto target = std::filesystem::path(dst) / rel;

            try
            {
                if (entry.is_directory())
                {
                    std::filesystem::create_directories(target);
                    std::cout << "[COPY DIR] " << target.string() << "\n";
                }
                else
                {
                    std::filesystem::copy_file(
                        entry.path(),
                        target,
                        std::filesystem::copy_options::overwrite_existing
                    );

                    std::cout << "[COPY FILE] " << entry.path().string()
                              << " -> " << target.string() << "\n";
                }
            }
            catch (const std::exception& e)
            {
                std::cout << "[COPY ERROR] file: " 
                          << entry.path() 
                          << " msg: " << e.what() << "\n";
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cout << "[COPY ERROR] iterator failed: " << e.what() << "\n";
        return;
    }

    std::cout << "[COPY] done\n";
}

std::string get_device_name()
{
    char name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = sizeof(name);

    if (GetComputerNameA(name, &size))
        return std::string(name);

    return "UNKNOWN_PC";
}

bool is_folder_complete(const std::string& src_path, const std::string& dst_path) {
    try {
        if (!std::filesystem::exists(dst_path)) return false;

        // 1. 比較檔案數量
        auto src_iter = std::filesystem::recursive_directory_iterator(src_path);
        auto dst_iter = std::filesystem::recursive_directory_iterator(dst_path);
        
        size_t src_count = std::distance(std::filesystem::begin(src_iter), std::filesystem::end(src_iter));
        size_t dst_count = std::distance(std::filesystem::begin(dst_iter), std::filesystem::end(dst_iter));

        if (src_count != dst_count) return false;

        // 2. 比較每個檔案的大小
        for (const auto& entry : std::filesystem::recursive_directory_iterator(src_path)) {
            if (std::filesystem::is_regular_file(entry)) {
                // 取得相對路徑
                auto rel_path = std::filesystem::relative(entry.path(), src_path);
                auto target_path = std::filesystem::path(dst_path) / rel_path;

                if (!std::filesystem::exists(target_path) || 
                    std::filesystem::file_size(entry.path()) != std::filesystem::file_size(target_path)) {
                    return false;
                }
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

void com_monitor(const std::string& ip, Listener* listener)
{
    int copy_conuter = 0;
    std::string msg = "Heart Beat!!!";
    Talker talker(ip, 9000);
    while (true)
    {
        std::vector<int> current_coms;
        std::set<int> current_set;

        if (get_all_9008_comports(current_coms))
        {
            current_set.insert(current_coms.begin(), current_coms.end());

            {
                std::lock_guard<std::mutex> lock(com_mutex);

                for (int com : current_set)
                {
                    if (seen_comports.insert(com).second) {
                        new_com_queue.push(com);
                        std::cout << "[EVENT] New COM detected: COM" << com << "\n";
                        com_cv.notify_one();
                    }
                }

                for (auto it = seen_comports.begin(); it != seen_comports.end(); )
                {
                    if (current_set.find(*it) == current_set.end()) {
                        std::cout << "[EVENT] COM removed: COM" << *it << "\n";
                        it = seen_comports.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        }

        // if (!talker.send_msg(MSG_HEART_BEAT,
        //                 msg.c_str(),
        //                 msg.size()))
        // {
        //     std::cout << "[Send failed] MSG_HEART_BEAT\n";
        // }

        std::string device_name = get_device_name();

        if (!talker.send_msg(MSG_HEART_BEAT,
                            device_name.c_str(),
                            device_name.size()))
        {
            std::cout << "[Send failed] MSG_HEART_BEAT\n";
        }

        auto_flash = listener->auto_flash();
        if (!talker.send_msg(MSG_AUTO_FLASH_STATUS,
                            &auto_flash,
                            sizeof(auto_flash)))
        {
            std::cout << "[Send failed] MSG_AUTO_FLASH_STATUS\n";
        }
        

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

                    bool complete = is_folder_complete(download, installer);
                    if (!complete){
                        std::cout << "installer path: " << installer << " maybe incomplete!!!" << std::endl;

                    }
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

    // std::string ip = " 10.235.50.244";
    std::string ip = "100.86.11.122";
    // std::string ip = "192.168.1.106";

    std::thread com_monitor_thread(com_monitor,ip, &listener);
    com_monitor_thread.detach();
    std::string folder = "C:\\workspace\\Glymur\\r4900\\Installer";
    

    

    while (true)
    {
        int comport = -1;

        if (auto_flash)
        {
            if (!std::filesystem::exists(listener.installer_path))
            {
                std::cout << "CAN'T FIND INSTALLER!!!" << std::endl;
            }else{

                {
                    std::unique_lock<std::mutex> lock(com_mutex);
                    com_cv.wait(lock, [] {
                        return !new_com_queue.empty();
                    });

                    comport = new_com_queue.front();
                    new_com_queue.pop();
                }
                // check validity
                if (seen_comports.find(comport) != seen_comports.end())
                {            
                    std::cout << "[MAIN] Start flashing COM" << comport << "\n";
                    std::thread flash_thread(
                        flash_worker,
                        listener.installer_path,
                        comport
                    );
                    flash_thread.detach();
                }else{
                    std::cout << "[SKIP] COM not in seen_comports: " << comport << "\n";
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

    }

    
    


    
    return 1;
}