#ifdef _WIN32
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0601
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #include <shellapi.h>
#endif

#include "auto_flash/local_server.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>
#include "httplib.h"

#include "index_html.h"   // 建置時由 tools/embed_file.py 從 host_server/index.html 產生

using json = nlohmann::json;

namespace {

std::mutex g_mutex;
LocalMirrorState g_state;
bool g_have_state = false;

LocalConfigRequest g_pending;
bool g_have_pending = false;

std::unique_ptr<httplib::Server> g_server;
std::thread g_thread;
std::atomic<int> g_port{0};

const char* comport_status_str(ComportStatus s)
{
    switch (s) {
        case ComportStatus::PENDING:  return "pending";
        case ComportStatus::FLASHING: return "flashing";
        case ComportStatus::SUCCESS:  return "success";
        case ComportStatus::FAIL:     return "fail";
        case ComportStatus::REMOVED:  return "removed";
    }
    return "unknown";
}

const char* file_status_str(FileStatus s)
{
    switch (s) {
        case FileStatus::NOT_FOUND:         return "not_found";
        case FileStatus::FOUND:             return "found";
        case FileStatus::COPYING:           return "copying";
        case FileStatus::UNZIPPING:         return "unzipping";
        case FileStatus::COPY_COMPLETE:     return "copy_complete";
        case FileStatus::COPY_FAILED:       return "copy_failed";
        case FileStatus::PATH_NOT_A_FOLDER: return "path_not_a_folder";
        case FileStatus::WAITING_FOR_COPY:  return "waiting_for_copy";
        case FileStatus::MISSING_FILES:     return "missing_files";
    }
    return "unknown";
}

// 產生跟 host_server /clients 完全相同形狀的 JSON，只是陣列裡永遠只有這一台。
// 形狀一致是刻意的：這樣 host_server/index.html 可以原封不動直接用，不需要維護
// 第二份前端，UI 改一次兩邊同步。
std::string clients_json()
{
    LocalMirrorState s;
    bool have;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        s = g_state;
        have = g_have_state;
    }

    json arr = json::array();
    if (have) {
        json comports = json::object();
        for (const auto& c : s.comports) {
            // 跟 host_server 一致：已移除的 COM port 不列出來。
            if (c.status == ComportStatus::REMOVED) continue;
            comports[std::to_string(c.number)] = {
                {"number", c.number},
                {"status", comport_status_str(c.status)},
                {"log", c.log},
                {"error_msg", c.error_msg}
            };
        }

        json fi = {
            {"status", file_status_str(s.file_info.status)},
            {"total_bytes", s.file_info.total_bytes},
            {"copied_bytes", s.file_info.copied_bytes},
            {"progress", s.file_info.progress},
            {"speed_mbps", s.file_info.speed_mbps},
            {"eta_seconds", s.file_info.eta_seconds},
            {"error_msg", s.file_info.error_msg},
            {"warning_msg", s.file_info.warning_msg},
            {"current_file", s.file_info.current_file},
            {"total_files", s.file_info.total_files}
        };

        arr.push_back({
            {"ip", s.ip},
            {"server_ip", s.server_ip},
            {"server_name", s.server_name},
            {"device", s.device},
            {"heartbeat", s.heartbeat},
            {"auto_flash", s.auto_flash},
            {"installer_path", s.installer_path},
            {"download_path", s.download_path},
            {"chipset", chipset_to_string(s.chipset)},
            {"storage", storage_to_string(s.storage)},
            {"flash_stage", stage_to_string(s.flash_stage)},
            {"comport_list", comports},
            {"file_info", fi}
        });
    }
    return arr.dump();
}

void open_in_browser(int port)
{
#ifdef _WIN32
    std::string url = "http://127.0.0.1:" + std::to_string(port) + "/";
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    (void)port;
#endif
}

} // namespace

void local_server_publish(const LocalMirrorState& state)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state = state;
    g_have_state = true;
}

bool local_server_take_config(LocalConfigRequest& out)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_have_pending) return false;
    out = g_pending;
    g_pending = LocalConfigRequest{};
    g_have_pending = false;
    return true;
}

int local_server_port()
{
    return g_port.load();
}

bool local_server_start(int port, bool open_browser)
{
    if (port <= 0) return false;
    if (g_server) return true;

    g_server = std::make_unique<httplib::Server>();

    g_server->Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(kIndexHtml, "text/html");
    });

    g_server->Get("/clients", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(clients_json(), "application/json");
    });

    // 只回報這一台，但保留 /status 讓人用 curl 快速確認 server 活著。
    g_server->Get("/status", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("local auto_flash server alive", "text/plain");
    });

    g_server->Post("/send", [](const httplib::Request& req, httplib::Response& res) {
        LocalConfigRequest r;
        try {
            auto j = json::parse(req.body);

            // 語意跟遠端 /send 對齊：只改「有出現在 body 裡」的欄位，
            // 而且空字串不覆蓋既有設定（UI 會送出整份表單，含沒填的欄位）。
            if (j.contains("auto_flash"))
                r.auto_flash = (j["auto_flash"] == "1" || j["auto_flash"] == true);

            if (j.contains("installer_path")) {
                std::string p = strip_surrounding_quotes(j["installer_path"].get<std::string>());
                if (!p.empty()) r.installer_path = p;
            }
            if (j.contains("download_path")) {
                std::string p = strip_surrounding_quotes(j["download_path"].get<std::string>());
                if (!p.empty()) r.download_path = p;
            }
            if (j.contains("chipset") && !j["chipset"].get<std::string>().empty())
                r.chipset = chipset_from_string(j["chipset"]);
            if (j.contains("storage") && !j["storage"].get<std::string>().empty())
                r.storage = storage_from_string(j["storage"]);
            if (j.contains("flash_stage") && !j["flash_stage"].get<std::string>().empty())
                r.flash_stage = stage_from_string(j["flash_stage"]);
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(std::string("bad request: ") + e.what(), "text/plain");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            // 合併而不是覆蓋：連續兩次 POST（UI 的路徑欄位與開關是分開送的）
            // 不該讓後一次把前一次還沒被套用的欄位丟掉。
            if (r.auto_flash)     g_pending.auto_flash = r.auto_flash;
            if (r.installer_path) g_pending.installer_path = r.installer_path;
            if (r.download_path)  g_pending.download_path = r.download_path;
            if (r.chipset)        g_pending.chipset = r.chipset;
            if (r.storage)        g_pending.storage = r.storage;
            if (r.flash_stage)    g_pending.flash_stage = r.flash_stage;
            g_have_pending = true;
        }
        res.set_content("OK", "text/plain");
    });

    g_port = port;
    g_thread = std::thread([port]() {
        // 只綁 127.0.0.1。這個介面可以改燒錄路徑、開關 auto_flash，綁 0.0.0.0
        // 等於讓同網段任何人都能對這台機器下指令，風險跟便利性完全不對等。
        if (!g_server->listen("127.0.0.1", port)) {
            std::cout << "[LOCAL] Failed to listen on 127.0.0.1:" << port
                      << " (port in use?)" << std::endl;
            g_port = 0;
        }
    });

    // listen() 失敗是非同步的，等一下再判斷，避免馬上開瀏覽器指到死連結。
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    if (g_port.load() == 0) return false;

    std::cout << "[LOCAL] WebUI + API on http://127.0.0.1:" << port
              << "/ (loopback only)" << std::endl;
    if (open_browser) open_in_browser(port);
    return true;
}

void local_server_stop()
{
    if (!g_server) return;
    g_server->stop();
    if (g_thread.joinable()) g_thread.join();
    g_server.reset();
    g_port = 0;
}
