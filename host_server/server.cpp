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


#include "httplib.h"



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

// ===== CONFIG =====
constexpr int HTTP_PORT = 8080;

// 另一台 server（你已有的 C++ socket listener）
constexpr const char* REMOTE_SERVER_IP = "192.168.1.100";
constexpr int REMOTE_SERVER_PORT = 9000;

// 本地 socket listener（收 status）
constexpr int LOCAL_LISTEN_PORT = 9001;

// ===== Socket: receive status =====
void status_listener()
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(LOCAL_LISTEN_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(sock, (sockaddr*)&addr, sizeof(addr));
    listen(sock, 5);

    std::cout << "[STATUS] Listening on port " << LOCAL_LISTEN_PORT << "\n";

    while (true)
    {
        int client = accept(sock, nullptr, nullptr);
        if (client < 0) continue;

        char buf[64] = {};
        int n = read(client, buf, sizeof(buf));

        if (n > 0)
        {
            std::string msg(buf);
            if (msg.find("1") != std::string::npos)
                g_status.store(true);
            else
                g_status.store(false);

            std::cout << "[STATUS] Update: " << g_status.load() << "\n";
        }

        close(client);
    }
}

// ===== Socket: send input =====
void send_to_remote(const std::string& msg)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(REMOTE_SERVER_PORT);
    inet_pton(AF_INET, REMOTE_SERVER_IP, &remote.sin_addr);

    if (connect(sock, (sockaddr*)&remote, sizeof(remote)) == 0)
    {
        send(sock, msg.c_str(), msg.size(), 0);
        std::cout << "[SEND] " << msg << "\n";
    }

    close(sock);
}

int main()
{
    // ===== Start socket listener thread =====
    std::thread(status_listener).detach();

    // ===== HTTP Server =====
    httplib::Server svr;

    // Serve HTML
    
    svr.Get("/", [&](const httplib::Request&, httplib::Response& res) {
        std::string html = read_file("index.html");
        res.set_content(html, "text/html");
    });


    // Status API
    svr.Get("/status", [](const httplib::Request&, httplib::Response& res) {
        std::string json = std::string("{\"status\": ")
            + (g_status.load() ? "true" : "false")
            + "}";
        res.set_content(json, "application/json");
    });

    // Send API
    svr.Post("/send", [](const httplib::Request& req, httplib::Response& res) {
        send_to_remote(req.body);
        res.set_content("OK", "text/plain");
    });

    std::cout << "[HTTP] Server running on port " << HTTP_PORT << "\n";
    svr.listen("0.0.0.0", HTTP_PORT);
}