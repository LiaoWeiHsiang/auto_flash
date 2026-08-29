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
    // timeout_sec：send/recv 逾時秒數。heartbeat 這種高頻小封包應該用短逾時，
    // 這樣即使對端一時沒在讀，也能很快失敗重試，不會拖累存活判定的時間窗。
    Talker(const std::string& ip, int port, int timeout_sec = 30);

    bool connect_server();
    bool ensure_connected();
    bool send_msg(MsgType type, const void* data, uint32_t len);
    void close_conn();

private:
    std::string ip_;
    int port_;
    int timeout_sec_;
    socket_t sock_;
    bool connected_ = false;  // socket_t 在 Windows 上是無號型別，sock_ >= 0 恒真，不能拿來判斷是否已連線
};