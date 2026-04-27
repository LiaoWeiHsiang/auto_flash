#pragma once
#include <iostream>
#include <cstring>
#include "utils/socket/socket.h"
#include "common/type.h"

// void talker(const std::string& ip, int port, const std::string& msg);

// void talker(const std::string& ip, int port, MsgType type, const void* data, uint32_t len);
// void send_packet(uint8_t type, const void* data, uint32_t len);

// class Talker
// {
// public:
//     Talker(const std::string& ip, int port);

//     bool connect_server();
//     bool send_msg(MsgType type, const void* data, uint32_t len);
//     void close_conn();

// private:
//     bool ensure_connected();

//     std::string ip_;
//     int port_;
//     socket_t sock_;
// };


class Talker
{
public:
    Talker(const std::string& ip, int port);

    bool connect_server();
    bool ensure_connected();
    bool send_msg(MsgType type, const void* data, uint32_t len);
    void close_conn();

private:
    std::string ip_;
    int port_;
    socket_t sock_;
};