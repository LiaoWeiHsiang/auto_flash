#include "utils/listener/listener.h"

// 🔧 Linux 需要包含這個頭文件才有 TCP_KEEPIDLE 等常數
#ifndef _WIN32
    #include <netinet/tcp.h>
#endif

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

    std::cout << ts() << "[Listener] Listening on port " << port_ << std::endl;
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

        std::cout << ts() << "[Client Connected] "
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
    // 🔧 設置接收超時 (60 秒，適合 WAN)
    struct timeval tv;
    tv.tv_sec = 60;
    tv.tv_usec = 0;

    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO,
               (const char*)&tv, sizeof(tv));

    // 🔧 啟用 TCP Keep-Alive，防止長時間無數據時斷線
    int keepalive = 1;
    setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE,
               (const char*)&keepalive, sizeof(keepalive));

#ifdef _WIN32
    // Windows: 設置 Keep-Alive 參數
    tcp_keepalive ka_settings;
    ka_settings.onoff = 1;
    ka_settings.keepalivetime = 60000;      // 60 seconds
    ka_settings.keepaliveinterval = 10000;  // 10 seconds
    DWORD bytes_returned;
    WSAIoctl(client_fd, SIO_KEEPALIVE_VALS, &ka_settings, sizeof(ka_settings),
             nullptr, 0, &bytes_returned, nullptr, nullptr);
#else
    // Linux: 設置 Keep-Alive 參數
    int keepidle = 60;   // 60 秒後開始發送 keep-alive
    int keepintvl = 10;  // 每 10 秒發送一次
    int keepcnt = 6;     // 最多嘗試 6 次
    setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
    setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));
#endif

    while (running_)
    {
        PacketHeader header;

        // 嘗試收 header（會一直等，timeout 不會斷線）
        if (!recv_all(client_fd, &header, sizeof(header)))
        {
            std::cout << ts() << "[Client session ended] " << ip << std::endl;
            break;
        }

        header.length = ntohl(header.length);

        // length == 0 是合法的：例如 client 尚未設定 installer/download path
        // 時會回報空字串。只有超過上限才視為協定錯誤並斷線。
        if (header.length > 10 * 1024 * 1024)
        {
            std::cout << ts() << "[ERROR] Invalid packet length: " << header.length << std::endl;
            break;
        }

        std::vector<char> data(header.length);

        if (header.length > 0 && !recv_all(client_fd, data.data(), header.length))
        {
            std::cout << ts() << "[Client session ended during payload] " << ip << std::endl;
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
                // 只更新 client 回報的實際狀態，不覆蓋 server 設定的期望值
                // server_get_auto_flash_status 由 /send 設定，不應被 client 回報覆蓋
                c.auto_flash = value;
                server_get_auto_flash_status_ = c.server_get_auto_flash_status;
                std::cout.flush();
            }
            break;
        }

                        case MSG_COMPORT:
        {
            // Parse: int(4 bytes) + status(4 bytes) + log_length(4 bytes) + log_data
            if (length < 12) {
                std::cout << ts() << "[MSG_COMPORT] Invalid length: " << length << std::endl;
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
            offset += log_length;

            // Read error_msg（新欄位加在尾端，舊封包沒有這段就留空字串）
            if (offset + sizeof(uint32_t) <= length) {
                uint32_t error_msg_length;
                memcpy(&error_msg_length, data.data() + offset, sizeof(uint32_t));
                offset += sizeof(uint32_t);
                if (offset + error_msg_length <= length) {
                    info.error_msg = std::string(data.data() + offset, error_msg_length);
                }
            }

            auto& c = clients_map[ip];

            // ⚠️ 只在狀態真的改變時才印。這個 case 每 2 秒被呼叫一次，
            // 而 stdout 是無緩衝的：若 console 消費端（SSH terminal、pipe、
            // 重導向）讀得不夠快，write() 會阻塞，handle_client 的 recv
            // 迴圈跟著停住，client 的 send() 就會卡到 30 秒逾時而被判 DEAD。
            if (info.status == ComportStatus::REMOVED) {
                c.comport_list.erase(info.number);
                std::cout << ts() << "[COMPORT] COM" << info.number << " REMOVED" << std::endl;
            } else {
                auto it = c.comport_list.find(info.number);
                bool status_changed = (it == c.comport_list.end()) ||
                                      (it->second.status != info.status);
                c.comport_list[info.number] = info;
                if (status_changed) {
                    std::cout << ts() << "[COMPORT] COM" << info.number
                              << " status=" << (int)info.status << std::endl;
                }
            }

            break;
        }

        case MSG_PROGRESS:
        {
            if (length >= sizeof(int)) {
                int progress;
                memcpy(&progress, data.data(), sizeof(int));

                std::cout << ts() << "[PROGRESS] " << progress << std::endl;
                fflush(stdout);
            }
            break;
        }

        case MSG_LOG:
        {
            std::string log(data.begin(), data.end());
            std::cout << ts() << "[LOG] " << log << std::endl;
            fflush(stdout);
            break;
        }

        case MSG_SET_AUTO_FLASH:
        {
            if (length >= sizeof(int)) {
                int value = 0;
                memcpy(&value, data.data(), sizeof(int));

                // 🔧 log 必須印「剛收到的新值」。舊版在指派前就印
                // auto_flash_，印出來的其實是上一次的值，會讓人誤以為
                // 收到的是 1，實際上這次收到的可能是 0。
                std::cout << ts() << "[MSG_SET_AUTO_FLASH] " << value << std::endl;

                auto_flash_ = (value != 0);
                auto& c = clients_map[ip];  // auto-create if not exist
                c.auto_flash = (value != 0);
            }
            break;
        }

        case MSG_INSTALLER_PATH:
        {
            // 頭尾引號在這裡也擋一次：這是路徑真正被 std::filesystem 使用的地方，
            // 送過來的可能是舊版 server 或其他工具，不保證已經正規化過。
            std::string installer_path_ = strip_surrounding_quotes(
                std::string(data.begin(), data.end()));
            bool changed = (installer_path != installer_path_);
            installer_path = installer_path_;
            auto& c = clients_map[ip];  // auto-create if not exist
            c.installer_path = installer_path_;
            if (changed)
                std::cout << ts() << "[MSG_INSTALLER_PATH] " << installer_path_ << std::endl;
            break;
        }

                case MSG_DOWNLOAD_PATH:
        {
            std::string download_path_ = strip_surrounding_quotes(
                std::string(data.begin(), data.end()));
            bool changed = (download_path != download_path_);
            download_path = download_path_;
            auto& c = clients_map[ip];  // auto-create if not exist
            c.download_path = download_path_;
            if (changed)
                std::cout << ts() << "[MSG_DOWNLOAD_PATH] " << download_path_ << std::endl;
            break;
        }

                case MSG_FILE_STATUS:
        {
            if (length < 44) {  // Minimum size without error message
                std::cout << ts() << "[MSG_FILE_STATUS] Invalid length" << std::endl;
                break;
            }
            
            FileInfo info;
            size_t offset = 0;
            
            // Status (4 bytes)
            int status_int;
            memcpy(&status_int, data.data() + offset, sizeof(int));
            info.status = static_cast<FileStatus>(status_int);
            offset += sizeof(int);
            
            // Total bytes (8 bytes)
            memcpy(&info.total_bytes, data.data() + offset, sizeof(uint64_t));
            offset += sizeof(uint64_t);
            
            // Copied bytes (8 bytes)
            memcpy(&info.copied_bytes, data.data() + offset, sizeof(uint64_t));
            offset += sizeof(uint64_t);
            
            // Progress (8 bytes)
            memcpy(&info.progress, data.data() + offset, sizeof(double));
            offset += sizeof(double);
            
            // Speed (8 bytes)
            memcpy(&info.speed_mbps, data.data() + offset, sizeof(double));
            offset += sizeof(double);
            
            // ETA (4 bytes)
            memcpy(&info.eta_seconds, data.data() + offset, sizeof(int));
            offset += sizeof(int);
            
            // Error message length (4 bytes)
            uint32_t error_len;
            memcpy(&error_len, data.data() + offset, sizeof(uint32_t));
            offset += sizeof(uint32_t);
            
            // Error message
            if (offset + error_len <= length) {
                info.error_msg = std::string(data.data() + offset, error_len);
                offset += error_len;

                // Warning message（附加欄位，舊版 client 不會送）
                // 長度不夠就當作沒有，不要把舊 client 的封包判成錯誤。
                if (offset + sizeof(uint32_t) <= length) {
                    uint32_t warning_len;
                    memcpy(&warning_len, data.data() + offset, sizeof(uint32_t));
                    offset += sizeof(uint32_t);
                    if (offset + warning_len <= length) {
                        info.warning_msg = std::string(data.data() + offset, warning_len);
                        offset += warning_len;

                        // File counters（再附加的欄位，同樣要容忍舊 client）
                        if (offset + 2 * sizeof(int) <= length) {
                            memcpy(&info.current_file, data.data() + offset, sizeof(int));
                            offset += sizeof(int);
                            memcpy(&info.total_files, data.data() + offset, sizeof(int));
                            offset += sizeof(int);
                        }
                    }
                }
            }
            
            auto& c = clients_map[ip];
            FileStatus prev_status = c.file_info.status;
            c.file_info = info;

            // 只在狀態轉換時印，不要每 2 秒印一次進度（進度看 Web UI）
            if (prev_status != info.status) {
                std::cout << ts() << "[FILE_STATUS] " << (int)prev_status
                          << " -> " << (int)info.status << std::endl;
            }
            break;
        }

        case MSG_CLIENT_INSTALLER_PATH:
        {
            std::string client_installer_path(data.begin(), data.end());
            auto& c = clients_map[ip];
            if (c.installer_path != client_installer_path) {
                std::cout << ts() << "[MSG_CLIENT_INSTALLER_PATH] " << client_installer_path << std::endl;
            }
            c.installer_path = client_installer_path;
            break;
        }

        case MSG_CLIENT_DOWNLOAD_PATH:
        {
            std::string client_download_path(data.begin(), data.end());
            auto& c = clients_map[ip];
            if (c.download_path != client_download_path) {
                std::cout << ts() << "[MSG_CLIENT_DOWNLOAD_PATH] " << client_download_path << std::endl;
            }
            c.download_path = client_download_path;
            break;
        }

        case MSG_CHIPSET:
        {
            if (length >= sizeof(int)) {
                int value = 0;
                memcpy(&value, data.data(), sizeof(int));

                chipset = static_cast<Chipset>(value);
                // Unsupported chipset+storage combos are forced back to NVME.
                storage = clamp_storage(chipset, storage);

                auto& c = clients_map[ip];  // auto-create if not exist
                c.chipset = chipset;
                c.storage = storage;

                std::cout << ts() << "[MSG_CHIPSET] " << chipset_to_string(chipset) << std::endl;
                fflush(stdout);
            }
            break;
        }

        case MSG_STORAGE:
        {
            if (length >= sizeof(int)) {
                int value = 0;
                memcpy(&value, data.data(), sizeof(int));

                storage = clamp_storage(chipset, static_cast<StorageType>(value));

                auto& c = clients_map[ip];  // auto-create if not exist
                c.storage = storage;

                std::cout << ts() << "[MSG_STORAGE] " << storage_to_string(storage) << std::endl;
                fflush(stdout);
            }
            break;
        }

        case MSG_CLIENT_CHIPSET:
        {
            if (length >= sizeof(int)) {
                int value = 0;
                memcpy(&value, data.data(), sizeof(int));

                auto& c = clients_map[ip];
                Chipset incoming = static_cast<Chipset>(value);
                if (c.chipset != incoming) {
                    std::cout << ts() << "[MSG_CLIENT_CHIPSET] "
                              << chipset_to_string(incoming) << std::endl;
                }
                c.chipset = incoming;
            }
            break;
        }

        case MSG_CLIENT_STORAGE:
        {
            if (length >= sizeof(int)) {
                int value = 0;
                memcpy(&value, data.data(), sizeof(int));

                auto& c = clients_map[ip];
                StorageType incoming = static_cast<StorageType>(value);
                if (c.storage != incoming) {
                    std::cout << ts() << "[MSG_CLIENT_STORAGE] "
                              << storage_to_string(incoming) << std::endl;
                }
                c.storage = incoming;
            }
            break;
        }

        case MSG_FLASH_STAGE:
        {
            if (length >= sizeof(int)) {
                int value = 0;
                memcpy(&value, data.data(), sizeof(int));

                flash_stage = static_cast<FlashStage>(value);

                auto& c = clients_map[ip];  // auto-create if not exist
                c.flash_stage = flash_stage;

                std::cout << ts() << "[MSG_FLASH_STAGE] " << stage_to_string(flash_stage) << std::endl;
                fflush(stdout);
            }
            break;
        }

        case MSG_CLIENT_FLASH_STAGE:
        {
            if (length >= sizeof(int)) {
                int value = 0;
                memcpy(&value, data.data(), sizeof(int));

                auto& c = clients_map[ip];
                FlashStage incoming = static_cast<FlashStage>(value);
                if (c.flash_stage != incoming) {
                    std::cout << ts() << "[MSG_CLIENT_FLASH_STAGE] "
                              << stage_to_string(incoming) << std::endl;
                }
                c.flash_stage = incoming;
            }
            break;
        }

        default:
            std::cout << ts() << "[UNKNOWN TYPE] "
                      << (int)type << std::endl;
            break;
    }
}