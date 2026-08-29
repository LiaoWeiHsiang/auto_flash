# System Patterns

## 整體架構

```
┌─────────────────────────────────────────────────────────┐
│  Windows Client (auto_flash.exe)                        │
│                                                         │
│  main()                                                 │
│  ├── Listener (port 9000) ← 接收 server 指令            │
│  ├── com_monitor thread                                 │
│  │   ├── 掃描 9008 COM ports (每 100ms)                 │
│  │   ├── 發送 heartbeat / COM 狀態 / file 狀態          │
│  │   └── 觸發 copy_worker (installer 不存在時)          │
│  ├── copy_worker thread (按需啟動)                      │
│  │   ├── 複製資料夾 / 單一檔案                          │
│  │   └── 解壓縮 ZIP (透過 PowerShell)                   │
│  └── 主迴圈：等待 COM port → 啟動 flash_worker thread  │
│                                                         │
└──────────────┬──────────────────────────────────────────┘
               │ TCP port 9000 (Talker → Listener)
               │ 自訂二進位協定 (Packet: type + length + data)
               ▼
┌─────────────────────────────────────────────────────────┐
│  Linux/Windows Server (server)                          │
│                                                         │
│  main()                                                 │
│  ├── Listener (port 9000) ← 接收 client 上報            │
│  ├── StatusProcessor thread (每 100ms 檢查心跳)         │
│  └── HTTP Server (port 8080, cpp-httplib)               │
│      ├── GET  /         → index.html                    │
│      ├── GET  /clients  → JSON (所有 client 狀態)       │
│      ├── GET  /status   → JSON (auto_flash 狀態)        │
│      └── POST /send     → 下發指令給指定 client         │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## IPC 通訊模式：Talker / Listener

### 協定格式
```
[type: uint8_t][length: uint32_t (network byte order)][data: bytes]
```

### 訊息類型（MsgType）

| 訊息 | 方向 | 說明 |
|------|------|------|
| `MSG_HEART_BEAT` | Client → Server | 裝置名稱，每 100ms |
| `MSG_AUTO_FLASH_STATUS` | Client → Server | auto_flash 開關狀態 |
| `MSG_COMPORT` | Client → Server | COM port 狀態 + log |
| `MSG_FILE_STATUS` | Client → Server | Installer 複製/解壓縮進度 |
| `MSG_CLIENT_INSTALLER_PATH` | Client → Server | Client 目前的 installer 路徑 |
| `MSG_CLIENT_DOWNLOAD_PATH` | Client → Server | Client 目前的 download 路徑 |
| `MSG_SET_AUTO_FLASH` | Server → Client | 設定 auto_flash 開關 |
| `MSG_INSTALLER_PATH` | Server → Client | 設定 installer 路徑 |
| `MSG_DOWNLOAD_PATH` | Server → Client | 設定 download 路徑 |

### Talker（發送端）
- 每個 `Talker` 物件持有一個 TCP socket
- `ensure_connected()` 懶連接：第一次 `send_msg` 時才建立連線
- 連線失敗自動重試一次
- 超時設定：30 秒（send/recv）
- TCP Keep-Alive：60s idle, 10s interval, 6 retries

### Listener（接收端）
- 每個連入的 client 開一個 `handle_client` thread
- `dispatch()` 根據 `type` 分派訊息，更新 `clients_map`
- 接收超時：60 秒（WAN 友好）

## 鎖設計

### `com_mutex`
保護 `comport_map` 和 `new_com_queue`。

**優化原則**：網路 I/O 必須在鎖外執行。
- 先在鎖內複製資料 → 鎖外發送
- 鎖持有時間目標：< 10ms

### `file_info_mutex`
保護 `global_file_info`（FileInfo 結構）。

**狀態機**：
```
NOT_FOUND → COPYING → COPY_COMPLETE
                    → COPY_FAILED
NOT_FOUND ← (installer 資料夾被刪除)
NOT_FOUND → FOUND (installer 已存在，非 COPY_FAILED/COPY_COMPLETE 時)
```

**重要規則**：`com_monitor` 不得覆蓋 `COPY_FAILED` 狀態。

## ZIP 解壓縮模式

Windows 使用 PowerShell + `System.IO.Compression.ZipFile`：
1. 建立臨時 `.ps1` 腳本
2. 透過 `CreateProcessA` 執行，捕獲 stdout
3. 解析 `TOTAL_FILES:`, `TOTAL_BYTES:`, `PROGRESS:`, `COMPLETE` 行
4. 透過 `UnzipProgressCallback` 回報進度

Linux 使用系統 `unzip` 命令（無進度回報）。

## 燒錄流程

```
flash_worker(folder, comport)
├── flash_image(folder, comport, SPINOR)  ← 最多重試 1 次
│   └── run_emmcdl(...)
│       ├── 建立 pipe，啟動 emmcdl.exe
│       ├── 非阻塞讀取 stdout（每 50ms）
│       ├── 偵測 "The operation completed successfully"
│       ├── Log stall timeout: 10 分鐘
│       └── Overall timeout: SPINOR 10 分鐘, HLOS 20 分鐘
├── Sleep(1000ms)
└── flash_image(folder, comport, HLOS)
```

成功/失敗都更新 `comport_map[comport].status`，由 `com_monitor` 定期上報給 server。