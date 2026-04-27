// #include "listener/listener.h"
// // ======================
// // Server (listener)
// // ======================

// // void listener(int port)
// // {
// //     socket_t server_fd, client_fd;
// //     struct sockaddr_in addr;
// //     char buffer[BUFFER_SIZE];

// //     struct sockaddr_in client_addr;
// //     socklen_t client_addr_len = sizeof(client_addr);


// //     server_fd = socket(AF_INET, SOCK_STREAM, 0);
// //     if (server_fd < 0) {
// //         perror("socket");
// //         return;
// //     }

// //     int opt = 1;
// //     if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
// //                 (char*)&opt, sizeof(opt)) < 0) {
// //         perror("setsockopt");
// //         close_socket(server_fd);
// //         return;
// //     }

// //     addr.sin_family = AF_INET;
// //     addr.sin_addr.s_addr = INADDR_ANY;
// //     addr.sin_port = htons(port);

// //     if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
// //         perror("bind");
// //         close_socket(server_fd);
// //         return;
// //     }

// //     if (listen(server_fd, 5) < 0) {
// //         perror("listen");
// //         return;
// //     }

// //     std::cout << "[Listener] Listening on port " << port << std::endl;

// //     while (true) {
// //         client_fd = accept(server_fd,
// //                         (struct sockaddr*)&client_addr,
// //                         &client_addr_len);

// //         if (client_fd < 0) {
// //             perror("accept");
// //             continue;
// //         }

// //         // timeout 應該設在 client_fd
// //         struct timeval tv;
// //         tv.tv_sec = 5;
// //         tv.tv_usec = 0;

// //         if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO,
// //             (const char*)&tv, sizeof(tv)) < 0) {
// //             perror("setsockopt");
// //         }
// //         char ip[INET_ADDRSTRLEN];
// //         inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));

// //         int client_port = ntohs(client_addr.sin_port);

// //         memset(buffer, 0, BUFFER_SIZE);

// //     #ifdef _WIN32
// //         int n = recv(client_fd, buffer, BUFFER_SIZE, 0);
// //     #else
// //         int n = read(client_fd, buffer, BUFFER_SIZE);
// //     #endif


    
// //         if (n > 0) {
// //             std::cout << "[Received] " << buffer << std::endl;
// //             std::cout << "Client IP: " << ip
// //                     << " Port: " << client_port << std::endl;
// //         }

// //         close_socket(client_fd);
// //     }
// // }


// void listener(int port)
// {
//     socket_t server_fd, client_fd;
//     struct sockaddr_in addr;
//     struct sockaddr_in client_addr;
//     socklen_t client_addr_len = sizeof(client_addr);

//     char ip[INET_ADDRSTRLEN];

//     // ===== create socket =====
//     server_fd = socket(AF_INET, SOCK_STREAM, 0);
//     if (server_fd < 0) {
//         perror("socket");
//         return;
//     }

//     int opt = 1;
//     setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
//                 (const char*)&opt, sizeof(opt));

//     // ===== bind =====
//     addr.sin_family = AF_INET;
//     addr.sin_addr.s_addr = INADDR_ANY;
//     addr.sin_port = htons(port);

//     if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
//         perror("bind");
//         close_socket(server_fd);
//         return;
//     }

//     // ===== listen =====
//     if (listen(server_fd, 5) < 0) {
//         perror("listen");
//         close_socket(server_fd);
//         return;
//     }

//     std::cout << "[Listener] Listening on port " << port << std::endl;

//     // ==============================
//     // TLV header
//     // ==============================
// #pragma pack(push, 1)
//     struct PacketHeader {
//         uint8_t type;
//         uint32_t length;
//     };
// #pragma pack(pop)

//     while (true)
//     {
//         client_fd = accept(server_fd,
//                            (struct sockaddr*)&client_addr,
//                            &client_addr_len);

//         if (client_fd < 0) {
//             perror("accept");
//             continue;
//         }

//         inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
//         int client_port = ntohs(client_addr.sin_port);

//         std::cout << "[Client Connected] "
//                   << ip << ":" << client_port << std::endl;

//         // ===== timeout =====
//         struct timeval tv;
//         tv.tv_sec = 5;
//         tv.tv_usec = 0;

//         setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO,
//                    (const char*)&tv, sizeof(tv));

//         // ==============================
//         // 1. read header
//         // ==============================
//         // PacketHeader header;

//         // int n = recv(client_fd, (char*)&header, sizeof(header), 0);
//         // if (n <= 0) {
//         //     close_socket(client_fd);
//         //     continue;
//         // }

//         // ==============================
//         // 2. read payload (handle partial recv)
//         // ==============================
//         // char* data = new char[header.length];
//         // uint32_t received = 0;

//         // while (received < header.length)
//         // {
//         //     int r = recv(client_fd,
//         //                  data + received,
//         //                  header.length - received,
//         //                  0);

//         //     if (r <= 0) {
//         //         delete[] data;
//         //         close_socket(client_fd);
//         //         goto next_client;
//         //     }

//         //     received += r;
//         // }

//         // while (received < sizeof(header)) {
//         //     int r = recv(client_fd,
//         //                 ((char*)&header) + received,
//         //                 sizeof(header) - received,
//         //                 0);
//         //     if(r>0){
//         //         printf("r = %d\n", r);
//         //     }
//         //     if (r <= 0) {
//         //         close_socket(client_fd);
//         //         continue;
//         //     }

//         //     received += r;
//         // }

//         PacketHeader header;
//         uint32_t received = 0;

//         char* data; // = new char[header.length];
        

//         while (received < sizeof(header)) {
//             int r = recv(client_fd,
//                         ((char*)&header) + received,
//                         sizeof(header) - received,
//                         0);

//             if (r <= 0) {
//                 close_socket(client_fd);
//                 goto next_client;
//             }

//             received += r;
//         }

//         received = 0;
//         if (header.length == 0 || header.length > 10 * 1024 * 1024) {
//             std::cout << "Invalid length: " << header.length << std::endl;
//             close_socket(client_fd);
//             goto next_client;
//         }
//         data = new char[header.length];

//         while (received < header.length)
//         {
//             int r = recv(client_fd,
//                         data + received,
//                         header.length - received,
//                         0);

//             if (r <= 0) {
//                 delete[] data;
//                 close_socket(client_fd);
//                 goto next_client;
//             }

//             received += r;
//         }

//         // ==============================
//         // 3. dispatch
//         // ==============================
//         switch (header.type)
//         {
//             case MSG_COMPORT:
//             {
//                 int port;
//                 memcpy(&port, data, sizeof(int));
//                 std::cout << "[COMPORT] " << port << std::endl;
//                 break;
//             }

//             case MSG_PROGRESS:
//             {
//                 int progress;
//                 memcpy(&progress, data, sizeof(int));
//                 std::cout << "[PROGRESS] " << progress << std::endl;
//                 break;
//             }

//             case MSG_LOG:
//             {
//                 std::string log(data, header.length);
//                 std::cout << "[LOG] " << log << std::endl;
//                 break;
//             }

//             default:
//                 std::cout << "[UNKNOWN TYPE] " << (int)header.type << std::endl;
//                 break;
//         }

//         delete[] data;
//         close_socket(client_fd);

//     next_client:
//         continue;
//     }
// }



#include "utils/listener/listener.h"
#include <vector>
#include <iostream>
#include <cstring>
#include <atomic>

// ======================
// helper: recv all
// ======================
static bool recv_all(socket_t fd, void* buf, size_t len)
{
    size_t received = 0;

    while (received < len)
    {
        int r = recv(fd, (char*)buf + received, len - received, 0);
        if (r <= 0)
            return false;

        received += r;
    }

    return true;
}

// ======================
// Server (listener)
// ======================
// void listener(int port)
// {
//     socket_t server_fd, client_fd;
//     struct sockaddr_in addr;
//     struct sockaddr_in client_addr;
//     socklen_t client_addr_len = sizeof(client_addr);

//     char ip[INET_ADDRSTRLEN];

//     // ===== create socket =====
//     server_fd = socket(AF_INET, SOCK_STREAM, 0);
//     if (server_fd < 0) {
//         perror("socket");
//         return;
//     }

//     int opt = 1;
//     setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
//                (const char*)&opt, sizeof(opt));

//     // ===== bind =====
//     addr.sin_family = AF_INET;
//     addr.sin_addr.s_addr = INADDR_ANY;
//     addr.sin_port = htons(port);

//     if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
//         perror("bind");
//         close_socket(server_fd);
//         return;
//     }

//     // ===== listen =====
//     if (listen(server_fd, 5) < 0) {
//         perror("listen");
//         close_socket(server_fd);
//         return;
//     }

//     std::cout << "[Listener] Listening on port " << port << std::endl;

// #pragma pack(push, 1)
//     struct PacketHeader {
//         uint8_t type;
//         uint32_t length; // network order
//     };
// #pragma pack(pop)

//     while (true)
//     {
//         client_fd = accept(server_fd,
//                            (struct sockaddr*)&client_addr,
//                            &client_addr_len);

//         if (client_fd < 0) {
//             perror("accept");
//             continue;
//         }

//         inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
//         int client_port = ntohs(client_addr.sin_port);

//         std::cout << "[Client Connected] "
//                   << ip << ":" << client_port << std::endl;

//         // ===== timeout =====
//         struct timeval tv;
//         tv.tv_sec = 5;
//         tv.tv_usec = 0;

//         setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO,
//                    (const char*)&tv, sizeof(tv));

//         // ==============================
//         // 1. recv header
//         // ==============================
//         PacketHeader header;

//         if (!recv_all(client_fd, &header, sizeof(header))) {
//             close_socket(client_fd);
//             continue;
//         }

//         // endian convert
//         header.length = ntohl(header.length);

//         // ==============================
//         // 2. validate length
//         // ==============================
//         if (header.length == 0 || header.length > 10 * 1024 * 1024) {
//             std::cout << "[ERROR] Invalid length: "
//                       << header.length << std::endl;
//             close_socket(client_fd);
//             continue;
//         }

//         // ==============================
//         // 3. recv payload
//         // ==============================
//         std::vector<char> data(header.length);

//         if (!recv_all(client_fd, data.data(), header.length)) {
//             close_socket(client_fd);
//             continue;
//         }

//         // ==============================
//         // 4. dispatch
//         // ==============================
//         switch (header.type)
//         {
//             case MSG_COMPORT:
//             {
//                 if (header.length < sizeof(int)) break;

//                 int port;
//                 memcpy(&port, data.data(), sizeof(int));
//                 std::cout << "[COMPORT] " << port << std::endl;
//                 break;
//             }

//             case MSG_PROGRESS:
//             {
//                 if (header.length < sizeof(int)) break;

//                 int progress;
//                 memcpy(&progress, data.data(), sizeof(int));
//                 std::cout << "[PROGRESS] " << progress << std::endl;
//                 break;
//             }

//             case MSG_LOG:
//             {
//                 std::string log(data.begin(), data.end());
//                 std::cout << "[LOG] " << log << std::endl;
//                 break;
//             }

//             default:
//                 std::cout << "[UNKNOWN TYPE] "
//                           << (int)header.type << std::endl;
//                 break;
//         }

//         close_socket(client_fd);
//     }
// }
// extern std::atomic<bool> g_status;
extern bool auto_flash_status;
extern bool auto_flash;
void listener(int port)
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
    addr.sin_port = htons(port);

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

    std::cout << "[Listener] Listening on port " << port << std::endl;

#pragma pack(push, 1)
    struct PacketHeader {
        uint8_t type;
        uint32_t length; // network order
    };
#pragma pack(pop)

    while (true)
    {
        client_fd = accept(server_fd,
                           (struct sockaddr*)&client_addr,
                           &client_addr_len);

        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        int client_port = ntohs(client_addr.sin_port);

        std::cout << "[Client Connected] "
                  << ip << ":" << client_port << std::endl;

        // timeout（可保留）
        struct timeval tv;
        tv.tv_sec = 30;
        tv.tv_usec = 0;

        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO,
                   (const char*)&tv, sizeof(tv));

        while (true)
        {
            PacketHeader header;

            // 1. read header
            if (!recv_all(client_fd, &header, sizeof(header))) {
                std::cout << "[Client disconnected]\n";
                break;
            }

            header.length = ntohl(header.length);

            if (header.length == 0 || header.length > 10 * 1024 * 1024) {
                std::cout << "[ERROR] Invalid length\n";
                break;
            }

            // 2. read payload
            std::vector<char> data(header.length);

            if (!recv_all(client_fd, data.data(), header.length)) {
                std::cout << "[Client disconnected during payload]\n";
                break;
            }

            // 3. dispatch
            switch (header.type)
            {
                case MSG_HEART_BEAT:
                {
                    std::string log(data.begin(), data.end());
                    // std::cout << "[MSG_HEART_BEAT] " << log << std::endl;
                    fflush(stdout);
                    break;
                    // if (header.length >= sizeof(int)) {
                    //     int port;
                    //     memcpy(&port, data.data(), sizeof(int));
                    //     std::cout << "[COMPORT] " << port << std::endl;
                    //     std::cout.flush();
                    // }
                    // g_status = true;
                    // std::cout << "[MSG_HEART_BEAT] " <<  << std::endl;
                    break;
                }
                
                case MSG_AUTO_FLASH_STATUS:
                {
                    if (header.length >= sizeof(bool)) {
                        // int port;
                        memcpy(&auto_flash_status, data.data(), sizeof(bool));
                        // std::cout << "[MSG_AUTO_FLASH_STATUS] " << auto_flash_status << std::endl;
                        std::cout.flush();
                    }
                    break;
                }

                
                case MSG_COMPORT:
                {
                    if (header.length >= sizeof(int)) {
                        int port;
                        memcpy(&port, data.data(), sizeof(int));
                        std::cout << "[COMPORT] " << port << std::endl;
                        std::cout.flush();
                    }
                    break;
                }

                case MSG_PROGRESS:
                {
                    if (header.length >= sizeof(int)) {
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
                     if (header.length >= sizeof(int)) {
                        // int port;
                        // memcpy(&auto_flash, data.data(), sizeof(int));
                        // std::cout.flush();
                        int value = 0;
                        memcpy(&value, data.data(), sizeof(int));
                        auto_flash = (value != 0);

                        std::cout << "[MSG_SET_AUTO_FLASH] " << auto_flash << std::endl;
                        
                    }
                    break;
                }
                

                default:
                    std::cout << "[UNKNOWN TYPE] "
                              << (int)header.type << std::endl;
                    break;
            }
        }

        close_socket(client_fd);
    }
}