#include "protocol.h"

#include <cstring>
#include <iostream>

#ifndef _WIN32
    #include <fcntl.h>
    #include <errno.h>
#endif

// ======================
// socket init / close
// ======================
void init_socket()
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

void close_socket(socket_t s)
{
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

bool is_valid_socket(socket_t s)
{
#ifdef _WIN32
    return s != INVALID_SOCKET;
#else
    return s >= 0;
#endif
}

// ======================
// connect_with_timeout
// ======================
socket_t connect_with_timeout(const std::string& ip, int port, int timeout_sec)
{
    socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (!is_valid_socket(sock))
        return sock;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
        close_socket(sock);
#ifdef _WIN32
        return INVALID_SOCKET;
#else
        return -1;
#endif
    }

    // Switch to non-blocking so connect() returns immediately and we can
    // bound the wait with select() instead of relying on the (long) OS
    // default TCP connect timeout.
#ifdef _WIN32
    u_long nonblocking = 1;
    ioctlsocket(sock, FIONBIO, &nonblocking);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif

    int rc = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    bool in_progress = false;

    if (rc < 0) {
#ifdef _WIN32
        in_progress = (WSAGetLastError() == WSAEWOULDBLOCK);
#else
        in_progress = (errno == EINPROGRESS);
#endif
        if (!in_progress) {
            close_socket(sock);
#ifdef _WIN32
            return INVALID_SOCKET;
#else
            return -1;
#endif
        }
    }

    if (in_progress) {
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(sock, &write_set);

        struct timeval tv;
        tv.tv_sec = timeout_sec;
        tv.tv_usec = 0;

        int sel = select(static_cast<int>(sock) + 1, nullptr, &write_set, nullptr, &tv);
        if (sel <= 0) {
            // Timed out or select() error.
            close_socket(sock);
#ifdef _WIN32
            return INVALID_SOCKET;
#else
            return -1;
#endif
        }

        int so_error = 0;
        socklen_t len = sizeof(so_error);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&so_error, &len);
        if (so_error != 0) {
            close_socket(sock);
#ifdef _WIN32
            return INVALID_SOCKET;
#else
            return -1;
#endif
        }
    }

    // Restore blocking mode for the rest of the session (recv/send loops
    // below assume blocking sockets).
#ifdef _WIN32
    u_long blocking = 0;
    ioctlsocket(sock, FIONBIO, &blocking);
#else
    fcntl(sock, F_SETFL, flags);
#endif

    return sock;
}

// ======================
// send_all / recv_all
// ======================
bool send_all(socket_t fd, const void* buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int r = send(fd, (const char*)buf + sent, static_cast<int>(len - sent), 0);
        if (r <= 0)
            return false;
        sent += r;
    }
    return true;
}

bool recv_all(socket_t fd, void* buf, size_t len)
{
    size_t received = 0;
    while (received < len) {
        int r = recv(fd, (char*)buf + received, static_cast<int>(len - received), 0);
        if (r <= 0)
            return false;
        received += r;
    }
    return true;
}

// ======================
// packet framing: [type:1B][length:4B network order][payload]
// ======================
bool send_packet(socket_t fd, CmdMsgType type, const void* data, uint32_t len)
{
    uint32_t net_len = htonl(len);
    std::vector<char> buf(sizeof(uint8_t) + sizeof(uint32_t) + len);

    char* p = buf.data();
    *p++ = static_cast<uint8_t>(type);
    memcpy(p, &net_len, sizeof(uint32_t));
    p += sizeof(uint32_t);
    if (len > 0 && data != nullptr)
        memcpy(p, data, len);

    return send_all(fd, buf.data(), buf.size());
}

bool recv_packet(socket_t fd, CmdMsgType& type, std::vector<char>& data)
{
    uint8_t type_byte;
    uint32_t net_len;

    if (!recv_all(fd, &type_byte, sizeof(type_byte)))
        return false;
    if (!recv_all(fd, &net_len, sizeof(net_len)))
        return false;

    uint32_t len = ntohl(net_len);
    // Sanity cap: a single command's output chunk should never legitimately
    // exceed this; guards against a corrupted stream driving an unbounded
    // allocation.
    if (len > 64 * 1024 * 1024)
        return false;

    data.assign(len, '\0');
    if (len > 0 && !recv_all(fd, data.data(), len))
        return false;

    type = static_cast<CmdMsgType>(type_byte);
    return true;
}
