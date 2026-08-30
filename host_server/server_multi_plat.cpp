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
                    Chipset chipset,
                    StorageType storage,
                    FlashStage flash_stage,
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

    // ===== chipset / storage (always sent; they always have a valid value) =====
    int chipset_value = static_cast<int>(chipset);
    if (!talker.send_msg(MSG_CHIPSET, &chipset_value, sizeof(chipset_value))){
        std::cout << "[Send failed] MSG_CHIPSET\n";
    }
    else{
        std::cout << "[Send successful] MSG_CHIPSET: "
                  << chipset_to_string(chipset) << std::endl;
    }

    int storage_value = static_cast<int>(storage);
    if (!talker.send_msg(MSG_STORAGE, &storage_value, sizeof(storage_value))){
        std::cout << "[Send failed] MSG_STORAGE\n";
    }
    else{
        std::cout << "[Send successful] MSG_STORAGE: "
                  << storage_to_string(storage) << std::endl;
    }

    int stage_value = static_cast<int>(flash_stage);
    if (!talker.send_msg(MSG_FLASH_STAGE, &stage_value, sizeof(stage_value))){
        std::cout << "[Send failed] MSG_FLASH_STAGE\n";
    }
    else{
        std::cout << "[Send successful] MSG_FLASH_STAGE: "
                  << stage_to_string(flash_stage) << std::endl;
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

            // 🔧 增加超時時間到 5 秒，避免網路延遲誤判
            if (diff > 5)
            {
                if (c.heartbeat) {
                    std::cout << "[TIMEOUT] Client " << it->first << " marked as DEAD (last seen " << diff << "s ago)\n";
                }
                c.heartbeat = false;
            }
            else
            {
                if (!c.heartbeat) {
                    std::cout << "[RECOVERY] Client " << it->first << " marked as ALIVE\n";
                }
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
    // 🔧 禁用 stdout 緩衝，讓 log 即時輸出
    std::cout.setf(std::ios::unitbuf);
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

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

        // ⚠️ 鎖內只做淺複製，JSON 組裝（尤其是每個 comport 的 log，最多
        // 50KB）搬到鎖外做。這個 handler 每 500ms 被 Web UI 打一次，若在
        // clients_mutex 持鎖期間做大量字串/JSON 工作，會讓 client 端的
        // MSG_HEART_BEAT dispatch（唯一會等這把鎖的訊息）延遲，進而讓
        // handle_client 的 recv 迴圈跟著卡住、TCP 接收緩衝區塞滿，client
        // 端的 send() 被流量控制阻塞好幾秒，heartbeat 因此被判 DEAD。
        struct ClientSnapshot {
            std::string ip;
            std::string device_name;
            bool auto_flash;
            bool heartbeat;
            std::string installer_path;
            std::string download_path;
            Chipset chipset;
            StorageType storage;
            FlashStage flash_stage;
            std::unordered_map<int, ComportInfo> comport_list;
            FileInfo file_info;
        };

        std::vector<ClientSnapshot> snapshots;
        {
            std::lock_guard<std::mutex> lock(listener.clients_mutex);
            snapshots.reserve(listener.clients_map.size());
            for (auto& [ip, c] : listener.clients_map)
            {
                ClientSnapshot snap;
                snap.ip = ip;
                snap.device_name = c.device_name;
                snap.auto_flash = c.auto_flash.load();
                snap.heartbeat = c.heartbeat;
                snap.installer_path = c.installer_path;
                snap.download_path = c.download_path;
                snap.chipset = c.chipset;
                snap.storage = c.storage;
                snap.flash_stage = c.flash_stage;
                snap.comport_list = c.comport_list;  // COW/深拷貝，但不做 JSON 工作
                snap.file_info = c.file_info;
                snapshots.push_back(std::move(snap));
            }
        }

        json arr = json::array();

        for (auto& c : snapshots)
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
                    case ComportStatus::REMOVED:  continue;  // Skip removed ports
                    default:                      status_str = "unknown";  break;
                }

                comport_json[std::to_string(com_num)] = {
                    {"number", com_num},
                    {"status", status_str},
                    {"log", com_info.log},  // Add log
                    {"error_msg", com_info.error_msg}
                };
            }

            // Convert file_info to JSON
            std::string file_status_str;
            switch (c.file_info.status) {
                case FileStatus::NOT_FOUND:          file_status_str = "not_found";          break;
                case FileStatus::FOUND:              file_status_str = "found";              break;
                case FileStatus::COPYING:            file_status_str = "copying";            break;
                case FileStatus::UNZIPPING:          file_status_str = "unzipping";          break;
                case FileStatus::COPY_COMPLETE:      file_status_str = "copy_complete";      break;
                case FileStatus::COPY_FAILED:        file_status_str = "copy_failed";        break;
                case FileStatus::PATH_NOT_A_FOLDER:  file_status_str = "path_not_a_folder";  break;
                case FileStatus::WAITING_FOR_COPY:   file_status_str = "waiting_for_copy";   break;
                case FileStatus::MISSING_FILES:      file_status_str = "missing_files";      break;
                default:                             file_status_str = "unknown";            break;
            }

            json file_info_json = {
                {"status", file_status_str},
                {"total_bytes", c.file_info.total_bytes},
                {"copied_bytes", c.file_info.copied_bytes},
                {"progress", c.file_info.progress},
                {"speed_mbps", c.file_info.speed_mbps},
                {"eta_seconds", c.file_info.eta_seconds},
                {"error_msg", c.file_info.error_msg},
                {"warning_msg", c.file_info.warning_msg},
                {"current_file", c.file_info.current_file},
                {"total_files", c.file_info.total_files}
            };

            arr.push_back({
                {"ip", c.ip},
                {"device", c.device_name},
                {"auto_flash", c.auto_flash},
                {"heartbeat", c.heartbeat},
                {"installer_path", c.installer_path},
                {"download_path", c.download_path},
                {"chipset", chipset_to_string(c.chipset)},
                {"storage", storage_to_string(c.storage)},
                {"flash_stage", stage_to_string(c.flash_stage)},
                {"comport_list", comport_json},
                {"file_info", file_info_json}
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

        // 路徑先去掉頭尾的引號再存。使用者常直接貼上 Windows「複製檔案位址」
        // 的結果（"C:\path"），帶著引號的路徑到 client 會被當成不存在。
        // 先正規化再判斷是否為空，這樣只輸入 "" 也不會覆蓋掉原本的設定。
        if (j.contains("installer_path")) {
            std::string p = strip_surrounding_quotes(j["installer_path"].get<std::string>());
            if (!p.empty()) c.installer_path = p;
        }

        if (j.contains("download_path")) {
            std::string p = strip_surrounding_quotes(j["download_path"].get<std::string>());
            if (!p.empty()) c.download_path = p;
        }

        if (j.contains("chipset") && !j["chipset"].get<std::string>().empty())
            c.chipset = chipset_from_string(j["chipset"]);

        if (j.contains("storage") && !j["storage"].get<std::string>().empty())
            c.storage = storage_from_string(j["storage"]);

        if (j.contains("flash_stage") && !j["flash_stage"].get<std::string>().empty())
            c.flash_stage = stage_from_string(j["flash_stage"]);

        // Hamoa / Glymur are NVME only. Clamp here too, not just in the UI —
        // /send can be POSTed by anything.
        c.storage = clamp_storage(c.chipset, c.storage);

        // ===== send command (ONLY ON CHANGE OR ALWAYS OK) =====
        // Copy out of `c` before calling: set_auto_flash() takes these by
        // const&, and Talker's connect() inside it blocks long enough for
        // the client's own heartbeat thread (MSG_CLIENT_INSTALLER_PATH /
        // MSG_CLIENT_DOWNLOAD_PATH) to overwrite clients_map[ip] with its
        // stale value first — sending back exactly what we just tried to
        // change. Local copies break that aliasing.
        std::string auto_flash_str = std::to_string(c.server_get_auto_flash_status);
        std::string installer_path_copy = c.installer_path;
        std::string download_path_copy = c.download_path;
        Chipset chipset_copy = c.chipset;
        StorageType storage_copy = c.storage;
        FlashStage flash_stage_copy = c.flash_stage;

        set_auto_flash(
            auto_flash_str,
            installer_path_copy,
            download_path_copy,
            chipset_copy,
            storage_copy,
            flash_stage_copy,
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