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

#include "utils/talker/talker.h"
#include "utils/listener/listener.h"
#include "httplib.h"

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
std::atomic<bool> g_status{false};
bool auto_flash_status{false};
bool auto_flash = false;
// ===== CONFIG =====
constexpr int HTTP_PORT = 8080;

// // 另一台 server（你已有的 C++ socket listener）
// constexpr const char* REMOTE_SERVER_IP = "192.168.1.100";
// constexpr int REMOTE_SERVER_PORT = 9000;

// // 本地 socket listener（收 status）
// constexpr int LOCAL_LISTEN_PORT = 9001;

// // ===== Socket: receive status =====
// void status_listener()
// {
//     int sock = socket(AF_INET, SOCK_STREAM, 0);

//     sockaddr_in addr{};
//     addr.sin_family = AF_INET;
//     addr.sin_port = htons(LOCAL_LISTEN_PORT);
//     addr.sin_addr.s_addr = INADDR_ANY;

//     bind(sock, (sockaddr*)&addr, sizeof(addr));
//     listen(sock, 5);

//     std::cout << "[STATUS] Listening on port " << LOCAL_LISTEN_PORT << "\n";

//     while (true)
//     {
//         int client = accept(sock, nullptr, nullptr);
//         if (client < 0) continue;

//         char buf[64] = {};
//         int n = read(client, buf, sizeof(buf));

//         if (n > 0)
//         {
//             std::string msg(buf);
//             if (msg.find("1") != std::string::npos)
//                 g_status.store(true);
//             else
//                 g_status.store(false);

//             std::cout << "[STATUS] Update: " << g_status.load() << "\n";
//         }

//         close(client);
//     }
// }
void set_auto_flash(const std::string& msg, const std::string& ip)
{
    std::cout << "Send message from server" << std::endl;
    int value = std::stoi(msg);
    Talker talker(ip, 9000);
    if (!talker.send_msg(MSG_SET_AUTO_FLASH,
                        &value,
                        sizeof(value)))
    {
        std::cout << "[Send failed] MSG_SET_AUTO_FLASH\n";
    }
}



// ===== Socket: send input =====
// void send_to_remote(const std::string& msg)
// {
//     int sock = socket(AF_INET, SOCK_STREAM, 0);

//     sockaddr_in remote{};
//     remote.sin_family = AF_INET;
//     remote.sin_port = htons(REMOTE_SERVER_PORT);
//     inet_pton(AF_INET, REMOTE_SERVER_IP, &remote.sin_addr);

//     if (connect(sock, (sockaddr*)&remote, sizeof(remote)) == 0)
//     {
//         send(sock, msg.c_str(), msg.size(), 0);
//         std::cout << "[SEND] " << msg << "\n";
//     }

//     close(sock);
// }

// bool global_status = false;

int main()
{
    // ===== Start socket listener thread =====
    // std::thread(status_listener).detach();
    int listen_port = 9000;
    std::thread listener_thread(listener, listen_port);
    listener_thread.detach();

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
    svr.Get("/status", [](const httplib::Request&, httplib::Response& res) {
        std::string json = std::string("{\"status\": ")
            + (auto_flash_status ? "true" : "false")
            + "}";
        res.set_content(json, "application/json");
    });

    // Send API
    // svr.Post("/send", [](const httplib::Request& req, httplib::Response& res) {
    //     // send_to_remote(req.body);
    //     set_auto_flash(req.body, ip);
    //     res.set_content("OK", "text/plain");
    // });

    svr.Post("/send", [ip](const httplib::Request& req, httplib::Response& res) {
        set_auto_flash(req.body, ip);
        res.set_content("OK", "text/plain");
    });

    // std::cout << "[HTTP] Server running on port " << HTTP_PORT << "\n";
    svr.listen("0.0.0.0", HTTP_PORT);
}