#pragma once
#include <iostream>
#include <cstring>
#include "common/type.h"
#include <unordered_map>
#include <algorithm>
#include "utils/socket/socket.h"

// #define BUFFER_SIZE 1024

// void listener(int port);


#include <vector>
#include <atomic>
#include <thread>
#include <mutex>

class Listener
{
public:
    Listener(int port);

    void start();
    void stop();

    // getters
    bool auto_flash() const;
    bool server_get_auto_flash_status() const;
    bool heart_beat() const;

    int heart_beat_count = 0;
    std::string installer_path = "";
    std::string download_path = "";
    std::vector<std::string> client_ip_list;
    std::unordered_map<std::string, ClientInfo> clients_map;
    std::mutex clients_mutex;

private:
#pragma pack(push, 1)
    struct PacketHeader {
        uint8_t type;
        uint32_t length;
    };
#pragma pack(pop)

    int port_;

    std::atomic<bool> server_get_auto_flash_status_;
    std::atomic<bool> auto_flash_;

    std::thread worker_;
    std::atomic<bool> running_;
    
    // core
    void run();
    void handle_client(socket_t client_fd, std::string ip);
    void dispatch(uint8_t type, uint32_t length, const std::vector<char>& data, std::string ip);

    // helper
    static bool recv_all(socket_t fd, void* buf, size_t len);
};