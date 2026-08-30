#pragma once

#include <string>
#include <vector>
#include <optional>

#include "common/type.h"

// ============================================================================
// Client-side local WebUI / API
// ============================================================================
// auto_flash.exe 除了把狀態用封包送去遠端 host_server，同時也在 localhost 開一個
// HTTP server，公佈「一模一樣的資料」，並接受同樣的設定操作。這樣在機台本機就能
// 看狀態、改路徑、開關 auto_flash，不必依賴遠端 server 或網路。
//
// 為什麼要用「快照 + 待辦」而不是讓 HTTP 執行緒直接讀寫 listener 的欄位：
//   installer_path / download_path 是 std::string，com_monitor 每 100ms 就在讀它們。
//   多一個 HTTP 執行緒直接寫入等於多一組 std::string 的 data race（可能直接崩，
//   不只是讀到舊值）。改成：
//     - com_monitor 在「本來就要送封包」的地方，把同一份資料發佈成快照；
//       HTTP 執行緒只讀快照。
//     - HTTP 收到設定就丟進待辦區；由 com_monitor 自己（也就是原本就擁有這些欄位
//       讀取權的執行緒）取出並套用。
//   結果是新增的並發全部集中在兩個小結構上，不動既有的讀取路徑。

// 這個 client 對外公佈的完整狀態（等同封包內容）。
struct LocalMirrorState {
    std::string ip;
    std::string device;
    bool heartbeat = false;
    bool auto_flash = false;

    std::string installer_path;
    std::string download_path;
    Chipset chipset = Chipset::KENAI;
    StorageType storage = StorageType::NVME;
    FlashStage flash_stage = FlashStage::BOTH;

    FileInfo file_info;

    struct Comport {
        int number = 0;
        ComportStatus status = ComportStatus::PENDING;
        std::string log;
        std::string error_msg;
    };
    std::vector<Comport> comports;
};

// 本機 UI 送來、等 com_monitor 套用的設定。沒帶到的欄位維持原值（跟遠端 /send
// 的語意一致：只改收到的欄位）。
struct LocalConfigRequest {
    std::optional<bool> auto_flash;
    std::optional<std::string> installer_path;
    std::optional<std::string> download_path;
    std::optional<Chipset> chipset;
    std::optional<StorageType> storage;
    std::optional<FlashStage> flash_stage;
};

// com_monitor 呼叫：發佈最新狀態給本機 UI。
void local_server_publish(const LocalMirrorState& state);

// com_monitor 呼叫：取出待套用的設定。有東西可套用時回傳 true 並清空待辦。
bool local_server_take_config(LocalConfigRequest& out);

// 啟動／停止本機 server。port 為 0 表示不啟用。
// open_browser 為 true 時順便把預設瀏覽器指到這個位址。
bool local_server_start(int port, bool open_browser);
void local_server_stop();

// 目前實際監聽的 port（沒啟動則為 0），供 log／診斷使用。
int local_server_port();
