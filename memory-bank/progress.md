# Progress

## 已完成

### 核心功能
- [x] COM port 自動偵測（Windows SetupAPI，偵測 9008 裝置）
- [x] 多 COM port 並行燒錄（每個 COM port 獨立 thread）
- [x] SPINOR + HLOS 兩階段燒錄（`emmcdl.exe`）
- [x] 燒錄 log 即時串流（非阻塞 pipe 讀取，每 50ms）
- [x] 燒錄超時保護（SPINOR 10 分鐘，HLOS 20 分鐘，log stall 10 分鐘）

### 通訊與監控
- [x] Talker/Listener IPC 框架（自訂二進位協定）
- [x] 多 client 支援（server 可同時監控多台 Windows 機器）
- [x] Heartbeat 機制（5 秒超時判定 DEAD）
- [x] TCP Keep-Alive（防止長時間無資料斷線）
- [x] Web UI（即時顯示 client 狀態、COM port 狀態、燒錄 log）

### Installer 管理
- [x] 資料夾複製（遞迴，支援取消）
- [x] ZIP 解壓縮（Windows PowerShell，flatten 模式）
- [x] ZIP 解壓縮進度顯示（百分比、速度、ETA）
- [x] Installer 狀態機（NOT_FOUND / FOUND / COPYING / COPY_COMPLETE / COPY_FAILED）

### 效能優化
- [x] Lock 優化：網路 I/O 移出 mutex 範圍（鎖持有時間 < 25ms）
- [x] 單一 Talker 重用（避免頻繁建立/銷毀 socket）
- [x] File status 發送頻率降低（每 2 秒，而非每 100ms）

### Bug 修復
- [x] ZIP 解壓縮失敗後 UI 誤顯示 FOUND（`com_monitor` 覆蓋 `COPY_FAILED`）
  - 修復：`com_monitor` 不覆蓋 `COPY_FAILED`
  - 修復：解壓縮失敗時清理空資料夾
  - 修復：`send_file_status` 移到 lock 外
- [x] UNC 路徑（`\\server\share\...`）ZIP 解壓縮失敗
  - 根因：`std::filesystem::absolute()` 在 MinGW 中破壞 UNC 路徑前綴
  - 修復：偵測 UNC 路徑，跳過 `absolute()` 轉換（`utils/zip/unzip.cpp`）
  - 修復：確保 PowerShell 腳本中路徑使用反斜線
  - 修復：精確化錯誤偵測，避免誤判含 "Error" 的檔名

## 待完成

### 功能
- [ ] 重試機制：ZIP 解壓縮失敗後自動重試
- [ ] 手動觸發重新複製 installer（Web UI 按鈕）
- [ ] 燒錄完成後自動重啟裝置

### 品質
- [ ] `progress_cb` 中的 `send_file_status` 移到 lock 外
- [ ] Linux 版 ZIP 解壓縮進度回報
- [ ] 單元測試

### 部署
- [ ] 設定檔支援（目前 server IP 硬編碼在 `auto_flash.cpp`）
- [ ] 自動更新機制

## Git 歷史摘要

| Commit | 說明 |
|--------|------|
| `6a7e150` | [Feature] Show installer path, Optimize mutex, Add installer status |
| `d6c3a5f` | Add comport logs |
| `9a0da8c` | [Feature] Send comport information to server |
| `ba70ca1` | Bug fixed |
| `3891a32` | Enforce LF line endings |
| `757ce7f` | Add json |
| `c8f9978` | Add multi client feature, wrap function to class |
| `6df3b0f` | Refactor code, communication feature complete |