# Project Brief: auto_flash

## 專案目標

`auto_flash` 是一個**跨平台韌體自動燒錄系統**，用於自動偵測 Qualcomm 9008 模式的裝置（透過 COM port），並自動執行 SPINOR + HLOS 兩階段韌體燒錄流程。

## 核心需求

1. **自動偵測** — 持續掃描 COM port，偵測到 9008 裝置時自動加入燒錄佇列
2. **自動燒錄** — 使用 `emmcdl.exe` 執行 SPINOR → HLOS 兩階段燒錄
3. **遠端監控** — 透過 HTTP Web UI 即時查看所有 client 的燒錄狀態
4. **Installer 管理** — 支援從網路路徑複製或解壓縮 ZIP 格式的 installer 包
5. **多裝置並行** — 支援同時燒錄多個 COM port

## 系統邊界

- **Client（Windows）**：`auto_flash.exe` — 執行在目標機器上，負責偵測 COM port 和燒錄
- **Server（Linux/Windows）**：`server` — 執行在監控機器上，提供 Web UI 和指令下發

## 不在範圍內

- 韌體本身的製作
- 非 Qualcomm 9008 裝置的支援
- GUI 客戶端（只有 Web UI）