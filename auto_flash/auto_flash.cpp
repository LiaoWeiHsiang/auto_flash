#include <windows.h>
#include <iostream>
#include <string>
#include <chrono>
#include <atomic>
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

bool auto_flash_status{false};
bool auto_flash{false};

void com_monitor(const std::string& ip)
{
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
                        it = seen_comports.erase(it);  // 安全刪除
                    } else {
                        ++it;
                    }
                }
            }
        }

        if (!talker.send_msg(MSG_HEART_BEAT,
                        msg.c_str(),
                        msg.size()))
        {
            std::cout << "[Send failed] MSG_HEART_BEAT\n";
        }

        if (!talker.send_msg(MSG_AUTO_FLASH_STATUS,
                            &auto_flash,
                            sizeof(auto_flash)))
        {
            std::cout << "[Send failed] MSG_AUTO_FLASH_STATUS\n";
        }
        


        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
    }
}




int main(int argc, char* argv[])
{

    init_socket();
    // std::string ip = " 10.235.50.244";
    std::string ip = "100.86.11.122";
    std::thread com_monitor_thread(com_monitor,ip);
    com_monitor_thread.detach();
    std::string folder = "C:\\workspace\\Glymur\\r4900\\Installer";
    

    int listen_port = 9000;
    std::thread listener_thread(listener, listen_port);
    listener_thread.detach();

    while (true)
    {
        int comport = -1;

        {
            std::unique_lock<std::mutex> lock(com_mutex);
            com_cv.wait(lock, [] {
                return !new_com_queue.empty();
            });

            comport = new_com_queue.front();
            new_com_queue.pop();
        }

        std::cout << "[MAIN] Start flashing COM" << comport << "\n";

        if(auto_flash){
            // std::thread flash_thread(
            //     flash_worker,
            //     folder,
            //     comport
            // );
            // flash_thread.detach();
        }
    }

    
    


    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    return 1;
}