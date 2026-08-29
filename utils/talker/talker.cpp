#include "utils/talker/talker.h"
#include <vector>

// 🔧 Linux 需要包含這個頭文件才有 TCP_KEEPIDLE 等常數
#ifndef _WIN32
    #include <netinet/tcp.h>
#endif


static void print_socket_error()
{
#ifdef _WIN32
    int err = WSAGetLastError();
    std::cout << "[socket error] WSA = " << err << std::endl;
#else
    std::cout << "[socket error] errno = " << errno
              << " (" << strerror(errno) << ")" << std::endl;
#endif
}
bool send_all(socket_t fd, const void* buf, size_t len)
{
    size_t sent = 0;

    while (sent < len)
    {
        int r = send(fd, (const char*)buf + sent, len - sent, 0);
        if (r <= 0){
            print_socket_error();
            return false;
        }

        sent += r;
    }

    return true;
}


Talker::Talker(const std::string& ip, int port, int timeout_sec)
    : ip_(ip), port_(port), timeout_sec_(timeout_sec), sock_(-1), connected_(false)
{}

bool Talker::connect_server()
{
    sock_ = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
    if (sock_ == INVALID_SOCKET) return false;
#else
    if (sock_ < 0) return false;
#endif

    // 🔧 設置發送/接收超時
    struct timeval tv;
    tv.tv_sec = timeout_sec_;
    tv.tv_usec = 0;

#ifdef _WIN32
    DWORD timeout = static_cast<DWORD>(timeout_sec_) * 1000;  // milliseconds
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
#else
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#endif

    // 🔧 啟用 TCP Keep-Alive，防止長時間無數據時斷線
    int keepalive = 1;
    setsockopt(sock_, SOL_SOCKET, SO_KEEPALIVE, (const char*)&keepalive, sizeof(keepalive));

#ifdef _WIN32
    // Windows: 設置 Keep-Alive 參數
    // keepalivetime: 60 秒後開始發送 keep-alive
    // keepaliveinterval: 每 10 秒發送一次
    tcp_keepalive ka_settings;
    ka_settings.onoff = 1;
    ka_settings.keepalivetime = 60000;      // 60 seconds
    ka_settings.keepaliveinterval = 10000;  // 10 seconds
    DWORD bytes_returned;
    WSAIoctl(sock_, SIO_KEEPALIVE_VALS, &ka_settings, sizeof(ka_settings),
             nullptr, 0, &bytes_returned, nullptr, nullptr);
#else
    // Linux: 設置 Keep-Alive 參數
    int keepidle = 60;   // 60 秒後開始發送 keep-alive
    int keepintvl = 10;  // 每 10 秒發送一次
    int keepcnt = 6;     // 最多嘗試 6 次
    setsockopt(sock_, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
    setsockopt(sock_, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    setsockopt(sock_, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);

    if (inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr) <= 0) {
        close_socket(sock_);
        sock_ = -1;
        return false;
    }

    if (connect(sock_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close_socket(sock_);
        sock_ = -1;
        return false;
    }

    connected_ = true;
    std::cout << ts() << "[Connected]\n";
    return true;
}

bool Talker::ensure_connected()
{
    if (connected_)
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
    if (len > 0 && data != nullptr)
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
    if (connected_) {
        close_socket(sock_);
        sock_ = -1;
        connected_ = false;
    }
}