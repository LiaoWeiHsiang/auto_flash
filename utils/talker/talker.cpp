#include "utils/talker/talker.h"
#include <vector>

// // ======================
// // Client (talker)
// // ======================


// // void talker(const std::string& ip, int port, const std::string& msg)
// // {
// //     socket_t sock;
// //     struct sockaddr_in addr;

// //     sock = socket(AF_INET, SOCK_STREAM, 0);
// //     if (sock < 0) {
// //         perror("socket");
// //         return;
// //     }

// //     addr.sin_family = AF_INET;
// //     addr.sin_port = htons(port);

// //     if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
// //         std::cerr << "Invalid address\n";
// //         return;
// //     }

// //     if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
// //         perror("connect");
// //         return;
// //     }

// // #ifdef _WIN32
// //     send(sock, msg.c_str(), (int)msg.size(), 0);
// // #else
// //     send(sock, msg.c_str(), msg.size(), 0);
// // #endif

// //     std::cout << "[Sent] " << msg << std::endl;

// //     close_socket(sock);
// // }


// void talker(const std::string& ip, int port, MsgType type, const void* data, uint32_t len)
// {
//     socket_t sock;
//     struct sockaddr_in addr;

//     sock = socket(AF_INET, SOCK_STREAM, 0);
//     if (sock < 0) {
//         perror("socket");
//         return;
//     }

//     addr.sin_family = AF_INET;
//     addr.sin_port = htons(port);

//     if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
//         std::cerr << "Invalid address\n";
//         close_socket(sock);
//         return;
//     }

//     if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
//         perror("connect");
//         close_socket(sock);
//         return;
//     }

//     // =========================
//     // build packet
//     // =========================
//     uint32_t total_size = sizeof(uint8_t) + sizeof(uint32_t) + len;
//     char* buf = new char[total_size];

//     char* p = buf;

//     // type
//     *p = static_cast<uint8_t>(type);
//     p += sizeof(uint8_t);

//     // length
//     memcpy(p, &len, sizeof(uint32_t));
//     p += sizeof(uint32_t);

//     // data
//     memcpy(p, data, len);

//     // =========================
//     // send
//     // =========================
// #ifdef _WIN32
//     send(sock, buf, total_size, 0);
// #else
//     send(sock, buf, total_size, 0);
// #endif

//     std::cout << "[Sent] type=" << (int)type << " len=" << len << std::endl;

//     delete[] buf;
//     close_socket(sock);
// }

bool send_all(socket_t fd, const void* buf, size_t len)
{
    size_t sent = 0;

    while (sent < len)
    {
        int r = send(fd, (const char*)buf + sent, len - sent, 0);
        if (r <= 0)
            return false;

        sent += r;
    }

    return true;
}


// void talker(const std::string& ip, int port,
//             MsgType type, const void* data, uint32_t len)
// {
//     socket_t sock;
//     struct sockaddr_in addr;

//     sock = socket(AF_INET, SOCK_STREAM, 0);
//     if (sock < 0) {
//         perror("socket");
//         return;
//     }

//     addr.sin_family = AF_INET;
//     addr.sin_port = htons(port);

//     if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
//         std::cerr << "Invalid address\n";
//         close_socket(sock);
//         return;
//     }

//     if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
//         perror("connect");
//         close_socket(sock);
//         return;
//     }

//     uint32_t net_len = htonl(len);

//     uint32_t total_size = sizeof(uint8_t) + sizeof(uint32_t) + len;
//     std::vector<char> buf(total_size);

//     char* p = buf.data();

//     *p = static_cast<uint8_t>(type);
//     p += sizeof(uint8_t);

//     memcpy(p, &net_len, sizeof(uint32_t));
//     p += sizeof(uint32_t);

//     memcpy(p, data, len);

//     // send(sock, buf.data(), total_size, 0);
//     send_all(sock, buf.data(), total_size);

//     std::cout << "[Sent] type=" << (int)type
//               << " len=" << len << std::endl;

//     close_socket(sock);
// }



// class Talker
// {
// public:
//     Talker(const std::string& ip, int port)
//         : ip_(ip), port_(port), sock_(-1)
//     {}

//     bool connect_server()
//     {
//         sock_ = socket(AF_INET, SOCK_STREAM, 0);
//         if (sock_ < 0) return false;

//         sockaddr_in addr{};
//         addr.sin_family = AF_INET;
//         addr.sin_port = htons(port_);

//         if (inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr) <= 0) {
//             close_conn();
//             return false;
//         }

//         if (connect(sock_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
//             close_conn();
//             return false;
//         }

//         std::cout << "[Connected]\n";
//         return true;
//     }

//     bool ensure_connected()
//     {
//         if (sock_ >= 0)
//             return true;

//         return connect_server();
//     }

//     bool send_msg(MsgType type, const void* data, uint32_t len)
//     {
//         if (!ensure_connected())
//             return false;

//         uint32_t net_len = htonl(len);
//         uint32_t total_size = sizeof(uint8_t) + sizeof(uint32_t) + len;

//         std::vector<char> buf(total_size);
//         char* p = buf.data();

//         *p++ = static_cast<uint8_t>(type);
//         memcpy(p, &net_len, sizeof(uint32_t));
//         p += sizeof(uint32_t);
//         memcpy(p, data, len);

//         // 第一次送
//         if (!send_all(sock_, buf.data(), total_size))
//         {
//             std::cout << "[Send failed → reconnect]\n";

//             close_conn();

//             // 重連
//             if (!connect_server())
//                 return false;

//             // 再送一次
//             if (!send_all(sock_, buf.data(), total_size))
//             {
//                 std::cout << "[Send failed again]\n";
//                 close_conn();
//                 return false;
//             }
//         }

//         return true;
//     }

//     void close_conn()
//     {
//         if (sock_ >= 0) {
//             close_socket(sock_);
//             sock_ = -1;
//         }
//     }

// private:
//     std::string ip_;
//     int port_;
//     socket_t sock_;
// };




Talker::Talker(const std::string& ip, int port)
    : ip_(ip), port_(port), sock_(-1)
{}

bool Talker::connect_server()
{
    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);

    if (inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr) <= 0) {
        close_conn();
        return false;
    }

    if (connect(sock_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close_conn();
        return false;
    }

    std::cout << "[Connected]\n";
    return true;
}

bool Talker::ensure_connected()
{
    if (sock_ >= 0)
        return true;

    return connect_server();
}

bool Talker::send_msg(MsgType type, const void* data, uint32_t len)
{
    if (!ensure_connected())
        return false;

    uint32_t net_len = htonl(len);
    uint32_t total_size = sizeof(uint8_t) + sizeof(uint32_t) + len;

    std::vector<char> buf(total_size);
    char* p = buf.data();

    *p++ = static_cast<uint8_t>(type);
    memcpy(p, &net_len, sizeof(uint32_t));
    p += sizeof(uint32_t);
    memcpy(p, data, len);

    if (!send_all(sock_, buf.data(), total_size))
    {
        std::cout << "[Send failed, reconnect]\n";

        close_conn();

        if (!connect_server())
            return false;

        if (!send_all(sock_, buf.data(), total_size))
        {
            std::cout << "[Send failed again]\n";
            close_conn();
            return false;
        }
    }

    return true;
}

void Talker::close_conn()
{
    if (sock_ >= 0) {
        close_socket(sock_);
        sock_ = -1;
    }
}