// remote_cmd_host: sends one shell command to a remote_cmd_agent and
// streams back its output until the agent reports the command's exit code.
//
// Usage: remote_cmd_host <ip>[:port] "<command>"
#include "protocol.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

static constexpr int DEFAULT_PORT = 9200;
static constexpr int CONNECT_TIMEOUT_SEC = 5;

static void print_usage(const char* prog)
{
    std::cerr << "Usage: " << prog << " <ip>[:port] \"<command>\"\n"
              << "  default port: " << DEFAULT_PORT << "\n";
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    std::string target = argv[1];
    std::string ip = target;
    int port = DEFAULT_PORT;

    size_t colon = target.find(':');
    if (colon != std::string::npos) {
        ip = target.substr(0, colon);
        port = std::atoi(target.substr(colon + 1).c_str());
    }

    // Join remaining args with spaces so a caller who forgets to quote the
    // whole command still gets a reasonable result.
    std::string command = argv[2];
    for (int i = 3; i < argc; ++i) {
        command += " ";
        command += argv[i];
    }

    init_socket();

    socket_t sock = connect_with_timeout(ip, port, CONNECT_TIMEOUT_SEC);
    if (!is_valid_socket(sock)) {
        std::cerr << "[remote_cmd] Failed to connect to " << ip << ":" << port << "\n";
        return 1;
    }

    if (!send_packet(sock, CmdMsgType::CMD_REQUEST, command.data(), static_cast<uint32_t>(command.size()))) {
        std::cerr << "[remote_cmd] Failed to send command\n";
        close_socket(sock);
        return 1;
    }

    int exit_code = 1;
    bool got_exit = false;

    while (true) {
        CmdMsgType type;
        std::vector<char> data;

        if (!recv_packet(sock, type, data)) {
            if (!got_exit)
                std::cerr << "[remote_cmd] Connection closed before command finished\n";
            break;
        }

        if (type == CmdMsgType::CMD_OUTPUT) {
            if (!data.empty()) {
                fwrite(data.data(), 1, data.size(), stdout);
                fflush(stdout);
            }
        } else if (type == CmdMsgType::CMD_EXIT) {
            int32_t net_code;
            memcpy(&net_code, data.data(), sizeof(net_code));
            exit_code = static_cast<int32_t>(ntohl(static_cast<uint32_t>(net_code)));
            got_exit = true;
            break;
        }
    }

    close_socket(sock);
    return exit_code;
}
