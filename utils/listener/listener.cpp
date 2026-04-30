#include "utils/listener/listener.h"

// ======================
// constructor
// ======================
Listener::Listener(int port)
    : port_(port),
      server_get_auto_flash_status_(false),
      auto_flash_(false),
      running_(false)
{}

// ======================
// start / stop
// ======================
void Listener::start()
{
    if (running_) return;

    running_ = true;
    worker_ = std::thread(&Listener::run, this);
}

void Listener::stop()
{
    running_ = false;

    if (worker_.joinable())
        worker_.join();
}

// ======================
// getters
// ======================
bool Listener::auto_flash() const
{
    return auto_flash_;
}

bool Listener::server_get_auto_flash_status() const
{
    return server_get_auto_flash_status_;
}

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
// ======================
// recv_all helper
// ======================

bool Listener::recv_all(socket_t fd, void* buf, size_t len)
{
    size_t received = 0;

    while (received < len)
    {
        int r = recv(fd, (char*)buf + received, len - received, 0);

        if (r == 0)
        {
            // Peer closed connection
            std::cout << "[Client closed connection]\n";
            return false;
        }

        if (r < 0)
        {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT)
            {
                // ✅ WAN 正常情況：只是暫時沒資料
                continue;
            }
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                continue;
            }
#endif
            // 真正錯誤
            print_socket_error();
            return false;
        }

        // 正常收到資料
        received += r;
    }

    return true;
}


// ======================
// main thread loop
// ======================
void Listener::run()
{
    socket_t server_fd, client_fd;
    struct sockaddr_in addr;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    char ip[INET_ADDRSTRLEN];

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&opt, sizeof(opt));

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close_socket(server_fd);
        return;
    }

    if (listen(server_fd, 5) < 0) {
        perror("listen");
        close_socket(server_fd);
        return;
    }

    std::cout << "[Listener] Listening on port " << port_ << std::endl;
    fflush(stdout);
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;

    setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO,
               (const char*)&tv, sizeof(tv));

    while (running_)
    {
        client_fd = accept(server_fd,
                           (struct sockaddr*)&client_addr,
                           &client_addr_len);

        if (client_fd < 0)
            continue;

        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        int client_port = ntohs(client_addr.sin_port);

        std::cout << "[Client Connected] "
                  << ip << ":" << client_port << std::endl;

        // handle_client(client_fd);
        std::thread(&Listener::handle_client, this, client_fd, std::string(ip)).detach();

        // close_socket(client_fd);
    }

    close_socket(server_fd);
}

// ======================
// client handler
// ======================

void Listener::handle_client(socket_t client_fd, std::string ip)
{
    struct timeval tv;
    tv.tv_sec = 30;   // WAN 上完全 OK
    tv.tv_usec = 0;

    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO,
               (const char*)&tv, sizeof(tv));

    while (running_)
    {
        PacketHeader header;

        // 嘗試收 header（會一直等，timeout 不會斷線）
        if (!recv_all(client_fd, &header, sizeof(header)))
        {
            std::cout << "[Client session ended] " << ip << std::endl;
            break;
        }

        header.length = ntohl(header.length);

        if (header.length == 0 || header.length > 10 * 1024 * 1024)
        {
            std::cout << "[ERROR] Invalid packet length\n";
            break;
        }

        std::vector<char> data(header.length);

        if (!recv_all(client_fd, data.data(), header.length))
        {
            std::cout << "[Client session ended during payload] " << ip << std::endl;
            break;
        }

        dispatch(header.type, header.length, data, ip);
    }

    close_socket(client_fd);
}

// ======================
// dispatch
// ======================
void Listener::dispatch(uint8_t type, uint32_t length, const std::vector<char>& data, std::string ip)
{
    switch (type)
    {
        case MSG_HEART_BEAT:
        {
            std::string device_name(data.begin(), data.end());

            std::lock_guard<std::mutex> lock(clients_mutex);

            auto& c = clients_map[ip];  // auto-create if not exist

            c.device_name = device_name;
            c.heartbeat = true;
            c.last_seen = std::chrono::steady_clock::now();

            if (std::find(client_ip_list.begin(), client_ip_list.end(), ip)
                == client_ip_list.end())
            {
                client_ip_list.push_back(ip);
            }

            // std::cout << "[HEARTBEAT] " << ip << " => " << device_name << "\n";
            break;
        }

        case MSG_AUTO_FLASH_STATUS:
        {
            if (length >= sizeof(bool)) {
                bool value;
                memcpy(&value, data.data(), sizeof(bool));
                auto& c = clients_map[ip];  // auto-create if not exist
                c.server_get_auto_flash_status = value;
                server_get_auto_flash_status_ = value;
                std::cout.flush();
            }
            break;
        }

                        case MSG_COMPORT:
        {
            std::cout << "[MSG_COMPORT] Get" << std::endl;
            
            // Parse: int(4 bytes) + status(4 bytes) + log_length(4 bytes) + log_data
            if (length < 12) {
                std::cout << "[MSG_COMPORT] Invalid length: " << length << std::endl;
                break;
            }

            ComportInfo info;
            size_t offset = 0;
            
            // Read COM number
            memcpy(&info.number, data.data() + offset, sizeof(int));
            offset += sizeof(int);
            
            // Read status
            int status_int;
            memcpy(&status_int, data.data() + offset, sizeof(int));
            info.status = static_cast<ComportStatus>(status_int);
            offset += sizeof(int);
            
            // Read log length
            uint32_t log_length;
            memcpy(&log_length, data.data() + offset, sizeof(uint32_t));
            offset += sizeof(uint32_t);
            
            // Read log data
            if (offset + log_length <= length) {
                info.log = std::string(data.data() + offset, log_length);
            }
            
            auto& c = clients_map[ip];
            
            // If status is REMOVED, delete from comport_list
            if (info.status == ComportStatus::REMOVED) {
                c.comport_list.erase(info.number);
                std::cout << "[COMPORT] " << info.number << " REMOVED" << std::endl;
            } else {
                c.comport_list[info.number] = info;
                std::cout << "[COMPORT] " << info.number << std::endl;
                std::cout << "[COMPORT] Status: " << (int)info.status << std::endl;
                std::cout << "[COMPORT] Log length: " << info.log.size() << std::endl;
            }
            
            std::cout.flush();
            break;
        }

        case MSG_PROGRESS:
        {
            if (length >= sizeof(int)) {
                int progress;
                memcpy(&progress, data.data(), sizeof(int));

                std::cout << "[PROGRESS] " << progress << std::endl;
                fflush(stdout);
            }
            break;
        }

        case MSG_LOG:
        {
            std::string log(data.begin(), data.end());
            std::cout << "[LOG] " << log << std::endl;
            fflush(stdout);
            break;
        }

        case MSG_SET_AUTO_FLASH:
        {
            std::cout << "[MSG_SET_AUTO_FLASH] "
                          << auto_flash_ << std::endl;
            if (length >= sizeof(int)) {
                int value = 0;
                memcpy(&value, data.data(), sizeof(int));

                auto_flash_ = (value != 0);
                auto& c = clients_map[ip];  // auto-create if not exist
                c.auto_flash = (value != 0);
                // std::cout << "[MSG_SET_AUTO_FLASH] "
                //           << auto_flash_ << std::endl;
                std::cout << "[MSG_SET_AUTO_FLASH] Size > sizeof(int) " << std::endl;
            }
            break;
        }

        case MSG_INSTALLER_PATH:
        {
            std::cout << "[MSG_INSTALLER_PATH] " << installer_path << std::endl;
            std::string installer_path_(data.begin(), data.end());
            installer_path = installer_path_;
            auto& c = clients_map[ip];  // auto-create if not exist
            c.installer_path = installer_path_;
            std::cout << "[MSG_INSTALLER_PATH] Dispatched successfully" << std::endl;
            fflush(stdout);
            break;
        }

        case MSG_DOWNLOAD_PATH:
        {
            std::string download_path_(data.begin(), data.end());
            download_path = download_path_;
            auto& c = clients_map[ip];  // auto-create if not exist
            c.download_path = download_path_;
            std::cout << "[MSG_DOWNLOAD_PATH] " << download_path << std::endl;
            fflush(stdout);
            break;
        }

        default:
            std::cout << "[UNKNOWN TYPE] "
                      << (int)type << std::endl;
            break;
    }
}