#pragma once
// ======================
// Cross-platform socket include (self-contained copy, mirrors
// utils/socket/socket.h so remote_cmd has no dependency on the rest
// of the auto_flash tree).
// ======================
#ifdef _WIN32
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0601
    #endif
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

#include <cstdint>
#include <string>
#include <vector>

void init_socket();
void close_socket(socket_t s);

// Connect with a bounded timeout (OS default blocking connect can hang for
// a long time against an unreachable host). Returns INVALID socket_t value
// on failure (see connect_failed()).
socket_t connect_with_timeout(const std::string& ip, int port, int timeout_sec);
bool is_valid_socket(socket_t s);

// type + length(network order) + payload framing, matching the convention
// already used by utils/talker + utils/listener in this repo.
enum class CmdMsgType : uint8_t {
    CMD_REQUEST = 1,  // host -> agent : command string (UTF-8)
    CMD_OUTPUT  = 2,  // agent -> host : a chunk of merged stdout+stderr
    CMD_EXIT    = 3,  // agent -> host : int32_t exit code (network order), final message
};

bool send_all(socket_t fd, const void* buf, size_t len);
bool recv_all(socket_t fd, void* buf, size_t len);

bool send_packet(socket_t fd, CmdMsgType type, const void* data, uint32_t len);
// Returns false when the connection closed or errored.
bool recv_packet(socket_t fd, CmdMsgType& type, std::vector<char>& data);
