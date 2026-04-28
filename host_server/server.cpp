#include <iostream>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <fstream>
#include <sstream>
#include <filesystem>
#include <nlohmann/json.hpp>

#include "utils/talker/talker.h"
#include "utils/listener/listener.h"
#include "httplib.h"

using json = nlohmann::json;
bool client_heart_beat = false;

std::string get_exe_dir()
{
    char path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len == -1) return ".";
    path[len] = '\0';

    return std::filesystem::path(path).parent_path().string();
}

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


// ===== Global status =====

// ===== CONFIG =====
constexpr int HTTP_PORT = 8080;


// }
void set_auto_flash(const std::string& auto_flash,const std::string& installer_path, const std::string& download_path, const std::string& ip)
{
    // std::string tmp_string;
    std::cout << "Send message from server" << std::endl;
    int value = std::stoi(auto_flash);
    Talker talker(ip, 9000);
    if (!talker.send_msg(MSG_SET_AUTO_FLASH,
                        &value,
                        sizeof(value)))
    {
        std::cout << "[Send failed] MSG_SET_AUTO_FLASH\n";
    }

    // tmp_string = installer_path
    if (!talker.send_msg(MSG_INSTALLER_PATH,
                        installer_path.c_str(),
                        installer_path.size()))
    {
        std::cout << "[Send failed] MSG_INSTALLER_PATH\n";
    }

    // tmp_string = installer_path
    if (!talker.send_msg(MSG_DOWNLOAD_PATH,
                        download_path.c_str(),
                        download_path.size()))
    {
        std::cout << "[Send failed] MSG_DOWNLOAD_PATH\n";
    }

}

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
    void processLoop() {
        using namespace std::chrono;

        steady_clock::time_point last_change_time = steady_clock::now();

        while (running_) {


            if (listener->heart_beat_count > last_heart_beat_count) {
                client_heart_beat = true;
                last_change_time = steady_clock::now();
            } else {
                auto now = steady_clock::now();
                auto diff = duration_cast<seconds>(now - last_change_time).count();

                if (diff >= 1) {
                    client_heart_beat = false;
                }
            }

            last_heart_beat_count = listener->heart_beat_count;

            std::this_thread::sleep_for(milliseconds(100));
        }

        std::cout << "status_process_thread stopped" << std::endl;
    }

private:
    std::thread thread_;
    std::atomic<bool> running_;

    Listener* listener;   // ✔ 正確存 pointer

    int last_heart_beat_count = 0;
};




int main()
{
    // ===== Start socket listener thread =====
    int listen_port = 9000;
    Listener listener(listen_port);
    listener.start();
    

    StatusProcessor sp(&listener);

    sp.start();


    std::string ip = "100.83.43.17";
    // ===== HTTP Server =====
    httplib::Server svr;

    // Serve HTML
    
    svr.Get("/", [&](const httplib::Request&, httplib::Response& res) {

        static std::string exe_dir = get_exe_dir();
        std::string html = read_file(exe_dir + "/index.html");

        res.set_content(html, "text/html");
    });


    // Status API
    svr.Get("/status", [&listener](const httplib::Request&, httplib::Response& res) {
        std::string json = std::string("{\"status\": ")
            + (listener.server_get_auto_flash_status() ? "true" : "false")
            + "}";

        res.set_content(json, "application/json");
    });

    svr.Get("/client_status", [](const httplib::Request&, httplib::Response& res) {
        std::string json = std::string("{\"client\": ")
            + (client_heart_beat ? "true" : "false")
            + "}";

        res.set_content(json, "application/json");
    });

    // Send API
    // svr.Post("/send", [ip](const httplib::Request& req, httplib::Response& res) {
    //     set_auto_flash(req.body, ip);
    //     res.set_content("OK", "text/plain");
    // });

    svr.Post("/send", [&](const httplib::Request& req, httplib::Response& res) {

        std::cout << "==== /send HIT ====\n";
        std::cout << "body: " << req.body << "\n";

        try {
            auto j = json::parse(req.body);

            std::cout << "PARSE OK\n";

            std::string auto_flash = j.value("auto_flash", "");
            std::cout << "auto_flash: " << auto_flash << "\n";
            std::string installer_path = j.value("installer_path", "");
            std::cout << "installer: " << installer_path << "\n";
            std::string download_path = j.value("download_path", "");

            
            
            std::cout << "download: " << download_path << "\n";

            set_auto_flash(auto_flash, installer_path,download_path , ip);

        } catch (const std::exception& e) {
            std::cout << "[JSON PARSE ERROR] " << e.what() << "\n";
        }

        // json j = json::parse(req.body);

        // std::cout << "body: " << req.body << "\n";

        // bool auto_flash = j["auto_flash"];
        // std::string installer_path = j["installer_path"];
        // std::string download_path = j["download_path"];

        // std::cout << "auto_flash: " << auto_flash << "\n";
        // std::cout << "installer: " << installer_path << "\n";
        // std::cout << "download: " << download_path << "\n";

        // set_auto_flash(auto_flash, ip);

        res.set_content("OK", "text/plain");
    });

    // std::cout << "[HTTP] Server running on port " << HTTP_PORT << "\n";
    svr.listen("0.0.0.0", HTTP_PORT);
    listener.stop(); 
    sp.stop();
}