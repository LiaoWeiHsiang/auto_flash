// remote_cmd_agent: listens for incoming connections and, for each one,
// reads a single command, runs it through the local shell, streams the
// merged stdout+stderr back live, then reports the exit code and closes
// the connection. Runs forever, accepting one connection (== one command)
// after another (or concurrently, one thread per connection).
//
// Usage: remote_cmd_agent [port]
#include "protocol.h"
#include "process_exec.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

static constexpr int DEFAULT_PORT = 9200;

static std::mutex g_log_mutex;

static void log_line(const std::string& msg)
{
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::cout << msg << std::endl;
}

static void handle_connection(socket_t client_fd, std::string peer_ip)
{
    CmdMsgType type;
    std::vector<char> data;

    if (!recv_packet(client_fd, type, data) || type != CmdMsgType::CMD_REQUEST) {
        log_line("[AGENT] " + peer_ip + ": failed to read command request");
        close_socket(client_fd);
        return;
    }

    std::string command(data.begin(), data.end());
    log_line("[AGENT] " + peer_ip + ": running: " + command);

    int exit_code = run_command(command, [&](const char* chunk, size_t len) {
        send_packet(client_fd, CmdMsgType::CMD_OUTPUT, chunk, static_cast<uint32_t>(len));
    });

    int32_t net_code = static_cast<int32_t>(htonl(static_cast<uint32_t>(exit_code)));
    send_packet(client_fd, CmdMsgType::CMD_EXIT, &net_code, sizeof(net_code));

    log_line("[AGENT] " + peer_ip + ": exit code " + std::to_string(exit_code));

    close_socket(client_fd);
}

int main(int argc, char** argv)
{
    int port = DEFAULT_PORT;
    if (argc > 1)
        port = std::atoi(argv[1]);

    init_socket();

    socket_t server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (!is_valid_socket(server_fd)) {
        std::cerr << "[AGENT] Failed to create socket\n";
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[AGENT] Failed to bind port " << port << "\n";
        close_socket(server_fd);
        return 1;
    }

    if (listen(server_fd, 16) < 0) {
        std::cerr << "[AGENT] Failed to listen on port " << port << "\n";
        close_socket(server_fd);
        return 1;
    }

    std::cout << "[AGENT] Listening on port " << port << std::endl;

    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        socket_t client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (!is_valid_socket(client_fd))
            continue;

        char ip_buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_buf, sizeof(ip_buf));

        std::thread(handle_connection, client_fd, std::string(ip_buf)).detach();
    }
}
