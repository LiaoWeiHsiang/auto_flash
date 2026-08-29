# Tech Context

## 技術棧

| 層次 | 技術 |
|------|------|
| 語言 | C++17 |
| 建置系統 | CMake 3.15+ |
| HTTP 伺服器 | cpp-httplib（header-only，`host_server/httplib.h`） |
| JSON | nlohmann/json（`third_party/json/`） |
| 跨平台 Socket | 自製抽象層（`utils/socket/`） |
| Windows 交叉編譯 | MinGW-w64（`toolchain-mingw64.cmake`） |
| ZIP 解壓縮（Windows） | PowerShell + `System.IO.Compression.ZipFile` |
| ZIP 解壓縮（Linux） | 系統 `unzip` 命令 |
| COM port 偵測 | Windows SetupAPI（`setupapi.h`） |
| 韌體燒錄工具 | `emmcdl.exe`（外部工具，需放在 installer 資料夾） |

## 建置設定

### `config.env`（控制建置目標）
```bash
BUILD_AUTO_FLASH=ON        # Windows client
BUILD_FLASH_EXE=OFF        # 獨立燒錄工具（測試用）
BUILD_HOST_SERVER=ON       # Linux server
BUILD_HOST_SERVER_WINDOWS=ON  # Windows server
```

### 建置指令
```bash
./build.sh
# 輸出：
# build-win/auto_flash.exe   (Windows client)
# build-win/server.exe       (Windows server)
# build-linux/server         (Linux server)
# build-linux/index.html     (Web UI)
```

### CMake 目標

| 目標 | 平台 | 主要原始碼 |
|------|------|-----------|
| `auto_flash` | Windows (MinGW) | `auto_flash/auto_flash.cpp` + utils |
| `flash_exe` | Windows (MinGW) | `auto_flash/flash_exe.cpp` |
| `server` | Linux 或 Windows | `host_server/server_multi_plat.cpp` + utils |

所有 Windows 目標使用 `-static` 連結，產生獨立執行檔。

## 目錄結構

```
auto_flash/
├── auto_flash/
│   ├── auto_flash.cpp      # Client 主程式
│   ├── flash_exe.cpp       # 獨立燒錄工具
│   ├── comport/            # COM port 偵測（Windows SetupAPI）
│   └── flash/              # emmcdl 執行與 log 管理
├── host_server/
│   ├── server_multi_plat.cpp  # 跨平台 HTTP server（主要使用）
│   ├── server.cpp             # Linux-only server（舊版，保留）
│   └── index.html             # Web UI（單頁應用）
├── utils/
│   ├── socket/             # 跨平台 socket 抽象
│   ├── talker/             # TCP 發送端
│   ├── listener/           # TCP 接收端（多 client）
│   └── zip/                # ZIP 解壓縮（跨平台）
├── common/
│   └── type.h              # 共用型別（MsgType, ComportInfo, FileInfo 等）
├── third_party/
│   └── json/               # nlohmann/json
└── skills/                 # Agent skills（Matt Pocock skills framework）
```

## 網路架構

```
Client (Windows)          Server (Linux/Windows)
port 9000 (Listener)  ←── Talker (from server /send endpoint)
Talker               ───→ port 9000 (Listener)
                          port 8080 (HTTP, Web UI)
```

Client 和 Server 都同時有 Listener 和 Talker：
- Client Listener：接收 server 下發的設定指令
- Server Listener：接收 client 上報的狀態

## 重要常數

| 常數 | 值 | 說明 |
|------|-----|------|
| `HTTP_PORT` | 8080 | Web UI 埠號 |
| `listen_port` | 9000 | IPC 通訊埠號 |
| `SPINOR_TIMEOUT_SEC` | 600 | SPINOR 燒錄超時（10 分鐘） |
| `HLOS_TIMEOUT_SEC` | 1200 | HLOS 燒錄超時（20 分鐘） |
| `LOG_STALL_TIMEOUT_SEC` | 600 | Log 停止輸出超時（10 分鐘） |
| `MAX_RETRY` | 1 | 燒錄最大重試次數 |
| `MAX_LOG_SIZE` | 50KB | 每個 COM port 的 log 上限 |
| heartbeat timeout | 5 秒 | 超過 5 秒未收到 heartbeat 標記為 DEAD |