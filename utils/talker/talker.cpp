#include "utils/talker/talker.h"
#include <vector>


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


Talker::Talker(const std::string& ip, int port)
    : ip_(ip), port_(port), sock_(-1)
{}

bool Talker::connect_server()
{
    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ < 0) return false;

    // 🔧 設置發送超時 (5 秒)
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    
#ifdef _WIN32
    DWORD timeout = 5000;  // 5 seconds in milliseconds
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
#else
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#endif

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