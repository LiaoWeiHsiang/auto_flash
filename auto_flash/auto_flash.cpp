#include <windows.h>
#include <iostream>
#include <string>
#include <chrono>
#include "common/type.h"
#include "comport/comport.h"
#include "flash/flash.h"

#include <queue>
#include <set>
#include <mutex>
#include <condition_variable>

std::queue<int> new_com_queue;
std::set<int> seen_comports;
std::vector<int> current_coms;

std::mutex com_mutex;
std::condition_variable com_cv;





void com_monitor()
{
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

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}




int main(int argc, char* argv[])
{

    std::string ip = " 10.235.50.244";
    std::thread com_monitor_thread(com_monitor);
    com_monitor_thread.detach();
    std::string folder = "C:\\workspace\\Glymur\\r4900\\Installer";
    

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

        // std::thread flash_thread(
        //     flash_worker,
        //     folder,
        //     comport
        // );
        // flash_thread.detach();
    }

    
    


    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    return 1;
}