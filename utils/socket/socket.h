#pragma once
// ======================
// Cross-platform socket include
// ======================
#ifdef _WIN32
    #define _WIN32_WINNT 0x0601
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")

    typedef SOCKET socket_t;
#else
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <sys/socket.h>

    typedef int socket_t;
#endif

void init_socket();
void close_socket(socket_t s);