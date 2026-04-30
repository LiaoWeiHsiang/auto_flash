#include <iostream>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>

#include <fstream>
#include <sstream>
#include <filesystem>

#include <nlohmann/json.hpp>
#include "httplib.h"

#include "utils/talker/talker.h"
#include "utils/listener/listener.h"

using json = nlohmann::json;

bool client_heart_beat = false;

// ===================== PLATFORM =====================
#ifdef _WIN32
    #include <windows.h>
    #include <direct.h>
    #define PATH_MAX MAX_PATH
#else
    #include <unistd.h>
    #include <limits.h>
#endif

// ===================== EXE PATH =====================
std::string get_exe_dir()
{
#ifdef _WIN32
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    return std::filesystem::path(path).parent_path().string();
#else
    char path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len == -1) return ".";
    path[len] = '\0';
    return std::filesystem::path(path).parent_path().string();
#endif
}

// ===================== FILE =====================
std::string read_file(const std::string& path)
{
    std::ifstream ifs(path);
    if (!ifs) {
        return "<h1>index.html not found</h1>";
    }

    std::stringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

// ===================== CONFIG =====================
constexpr int HTTP_PORT = 8080;

// ===================== SEND =====================
void set_auto_flash(const std::string& auto_flash,
                    const std::string& installer_path,
                    const std::string& download_path,
                    const std::string& ip)
{
    std::cout << "Send message from server\n";

    int value = 0;
    try {
        value = std::stoi(auto_flash);
    } catch (...) {
        std::cout << "[WARN] invalid auto_flash value\n";
    }

    Talker talker(ip, 9000);

    if (!talker.send_msg(MSG_SET_AUTO_FLASH, &value, sizeof(value)))
        std::cout << "[Send failed] MSG_SET_AUTO_FLASH\n";

    if (!installer_path.empty()) {
        if (!talker.send_msg(MSG_INSTALLER_PATH,
                             installer_path.c_str(),
                             installer_path.size())){
            std::cout << "[Send failed] MSG_INSTALLER_PATH\n";
        }
        else{
            std::cout << "[Send successful] MSG_INSTALLER_PATH\n";
            std::cout << "MSG_INSTALLER_PATH: " << installer_path << std::endl;
            
        }
    }else{
        std::cout << "installer path is empty. Can't Send\n";
    }

    if (!download_path.empty()) {
        if (!talker.send_msg(MSG_DOWNLOAD_PATH,
                             download_path.c_str(),
                             download_path.size())){
            std::cout << "[Send failed] MSG_DOWNLOAD_PATH\n";
        }
        else{
            std::cout << "[Send successful] MSG_DOWNLOAD_PATH\n";
        }
    }else{
        std::cout << "download path is empty. Can't Send\n";
    }
}



// ===================== STATUS THREAD =====================
class StatusProcessor {
public:
    StatusProcessor(Listener* listener_)
        : running_(false), listener(listener_) {}

    void start() {
        running_ = true;
        thread_ = std::thread(&StatusProcessor::processLoop, this);
    }

    void stop() {
        running_ = false;
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    ~StatusProcessor() {
        stop();
    }


    
private:

    // void check_clients()
    // {
    //     using namespace std::chrono;

    //     auto now = steady_clock::now();

    //     std::lock_guard<std::mutex> lock(listener->clients_mutex);

    //     for (auto it = listener->clients_map.begin();
    //         it != listener->clients_map.end(); )
    //     {
    //         auto& ip = it->first;
    //         auto& c  = it->second;

    //         auto diff = duration_cast<seconds>(now - c.last_seen).count();

    //         if (diff > 1)
    //         {
    //             // std::cout << "[TIMEOUT] remove " << ip << "\n";

    //             // listener->client_ip_list.erase(
    //             //     std::remove(listener->client_ip_list.begin(),
    //             //                 listener->client_ip_list.end(),
    //             //                 ip),
    //             //     listener->client_ip_list.end()
    //             // );

    //             // it = listener->clients_map.erase(it);
    //             listener->clients_map[ip].heartbeat = false;
    //         }
    //         else
    //         {
    //             c.heartbeat = true;
    //             ++it;
    //         }
    //     }
    // }

    void check_clients()
    {
        using namespace std::chrono;

        auto now = steady_clock::now();

        std::lock_guard<std::mutex> lock(listener->clients_mutex);

        for (auto it = listener->clients_map.begin();
            it != listener->clients_map.end();
            ++it)
        {
            auto& c = it->second;

            auto diff = duration_cast<seconds>(now - c.last_seen).count();

            if (diff > 1)
            {
                c.heartbeat = false;
            }
            else
            {
                c.heartbeat = true;
            }
        }
    }

    void processLoop() {
        using namespace std::chrono;

        steady_clock::time_point last_change_time = steady_clock::now();

        while (running_) {

            // if (listener->heart_beat_count > last_heart_beat_count) {
            //     client_heahrt_beat = true;
            //     last_change_time = steady_clock::now();
            // } else {
            //     auto now = steady_clock::now();
            //     auto diff = duration_cast<seconds>(now - last_change_time).count();

            //     if (diff >= 1) {
            //         client_eart_beat = false;
            //     }
            // }

            // last_heart_beat_count = listener->heart_beat_count;
            check_clients();
            // if(listener->client_ip_list.size()!=0){
            //     auto& c = listener->clients_map[listener->client_ip_list[0]]; 
            //     std::cout << "device: " << c.device_name << std::endl;
            //     std::cout << "auto_flash: " << c.server_get_auto_flash_status << std::endl;
            //     std::cout << "heartbeat: " << c.heartbeat << std::endl;
            //     std::cout << "installer_path: " << c.installer_path << std::endl;
            //     std::cout << "download_path: " << c.download_path << std::endl;
                
            // }
  
            std::this_thread::sleep_for(milliseconds(100));
        }

        std::cout << "status_process_thread stopped\n";
    }

private:
    std::thread thread_;
    std::atomic<bool> running_;
    Listener* listener;
    int last_heart_beat_count = 0;
};

// ===================== MAIN =====================
int main()
{
    int listen_port = 9000;
    Listener listener(listen_port);
    listener.start();

    StatusProcessor sp(&listener);
    sp.start();

    // std::string ip = "100.83.43.17";
    // std::string ip = "192.168.1.106";

    httplib::Server svr;

    // ===== HTML =====
    svr.Get("/", [&](const httplib::Request&, httplib::Response& res) {
        static std::string exe_dir = get_exe_dir();
        std::string html = read_file(exe_dir + "/index.html");
        res.set_content(html, "text/html");
    });

    // ===== STATUS =====
    svr.Get("/status", [&listener](const httplib::Request&, httplib::Response& res) {
        std::string json_str = std::string("{\"status\": ")
            + (listener.server_get_auto_flash_status() ? "true" : "false")
            + "}";

        res.set_content(json_str, "application/json");
    });

    // svr.Get("/client_status", [](const httplib::Request&, httplib::Response& res) {
    //     std::string json_str = std::string("{\"client\": ")
    //         + (listener->client_map[listener->client_ip_list[0]].heartbeat ? "true" : "false")
    //         + "}";

    //     res.set_content(json_str, "application/json");
    // });

        svr.Get("/clients", [&](const httplib::Request&, httplib::Response& res) {

        json arr = json::array();

        std::lock_guard<std::mutex> lock(listener.clients_mutex);

        for (auto& [ip, c] : listener.clients_map)
        {
            // Convert comport_list to JSON
            json comport_json = json::object();
            for (const auto& [com_num, com_info] : c.comport_list)
            {
                std::string status_str;
                switch (com_info.status) {
                    case ComportStatus::PENDING:  status_str = "pending";  break;
                    case ComportStatus::FLASHING: status_str = "flashing"; break;
                    case ComportStatus::SUCCESS:  status_str = "success";  break;
                    case ComportStatus::FAIL:     status_str = "fail";     break;
                    default:                      status_str = "unknown";  break;
                }
                
                comport_json[std::to_string(com_num)] = {
                    {"number", com_num},
                    {"status", status_str}
                };
            }

            arr.push_back({
                {"ip", ip},
                {"device", c.device_name},
                {"auto_flash", c.server_get_auto_flash_status},
                {"heartbeat", c.heartbeat},
                {"installer_path", c.installer_path},
                {"download_path", c.download_path},
                {"comport_list", comport_json}
            });
        }

        res.set_content(arr.dump(), "application/json");
    });

   
    svr.Post("/send", [&](const httplib::Request& req, httplib::Response& res) {

        auto j = json::parse(req.body);

        std::string target_ip = j.value("ip", "");
        if (target_ip.empty()) return;

        auto it = listener.clients_map.find(target_ip);
        if (it == listener.clients_map.end()) return;

        auto& c = it->second;

        // ===== update state ONLY =====
        if (j.contains("auto_flash"))
            c.server_get_auto_flash_status = (j["auto_flash"] == "1");

        if (j.contains("installer_path") && !j["installer_path"].get<std::string>().empty())
            c.installer_path = j["installer_path"];

        if (j.contains("download_path") && !j["download_path"].get<std::string>().empty())
            c.download_path = j["download_path"];

        // ===== send command (ONLY ON CHANGE OR ALWAYS OK) =====
        set_auto_flash(
            std::to_string(c.server_get_auto_flash_status),
            c.installer_path,
            c.download_path,
            target_ip
        );

        res.set_content("OK", "text/plain");
    });

    std::cout << "[HTTP] Server running on port " << HTTP_PORT << "\n";
    fflush(stdout);
    svr.listen("0.0.0.0", HTTP_PORT);

    listener.stop();
    sp.stop();

    return 0;
}