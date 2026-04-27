#include "utils/socket/socket.h"

void close_socket(socket_t s)
{
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

void init_socket()
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
#endif
}