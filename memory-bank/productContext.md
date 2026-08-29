# Product Context

## 為什麼存在

工廠或實驗室環境中需要對大量 Qualcomm 裝置進行韌體燒錄。手動操作效率低、容易出錯。`auto_flash` 讓操作員只需插入裝置，系統自動完成燒錄，並透過 Web UI 遠端監控進度。

## 使用情境

### 典型工作流程

```
1. 操作員在 Web UI 設定：
   - Installer Path（本機 installer 目錄）
   - Download Path（網路 installer 來源，可為 ZIP 或資料夾）
   - 開啟 Auto Flash 開關

2. auto_flash.exe（Windows client）：
   - 若 installer 不存在 → 從 Download Path 複製/解壓縮
   - 持續掃描 COM port
   - 偵測到 9008 裝置 → 加入佇列 → 等待 installer 就緒 → 開始燒錄

3. Web UI 即時顯示：
   - Client 心跳狀態（ALIVE/DEAD）
   - Installer 複製進度（百分比、速度、ETA）
   - 每個 COM port 的燒錄狀態（PENDING/FLASHING/SUCCESS/FAIL）
   - 燒錄 log
```

### Installer 來源類型

| 類型 | 行為 |
|------|------|
| ZIP 檔案 | 解壓縮到 `installer_path/installer/` 子資料夾 |
| 資料夾 | 遞迴複製整個資料夾 |
| 單一檔案 | 直接複製 |

## 使用者角色

- **操作員**：透過 Web UI 設定路徑、開關 Auto Flash、監控狀態
- **工程師**：部署 server 和 client，設定網路連線