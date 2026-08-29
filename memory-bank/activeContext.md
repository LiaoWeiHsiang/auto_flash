# Active Context

## 目前工作重點

### 最新變更：ZIP 直接解壓縮到 installer_path（`auto_flash/auto_flash.cpp`）

**變更內容**：
- **之前**：ZIP 解壓縮到 `installer_path/installer/`（建立子資料夾）
- **現在**：ZIP 直接解壓縮到 `installer_path`（不建立子資料夾）

**觸發條件改變**：
- **之前**：檢查 `installer_path` 是否存在才觸發複製
- **現在**：ZIP 檔改用狀態判斷（`NOT_FOUND` 時才觸發），不檢查路徑存在
  ```cpp
  if (download_is_zip) {
      should_start_copy = (current_status == FileStatus::NOT_FOUND);
  } else {
      should_start_copy = !std::filesystem::exists(installer);
  }
  ```
- ZIP 副檔名判斷用字串比對（`.zip`/`.ZIP`），避免頻繁網路 I/O

**同步移除**：`com_monitor()` 中的 `installer/` 子資料夾檢查邏輯（已簡化為直接檢查 `installer_path`）

---

### 剛修復的 Bug：UNC 路徑 ZIP 解壓縮失敗（`utils/zip/unzip.cpp`）

**問題描述**：
ZIP 檔案位於 Windows 網路共享路徑（如 `\\snowcone\builds682\...\Installer.zip`）時，程式解壓縮失敗，但手動執行 PowerShell 可以成功。

**根本原因**：
`std::filesystem::absolute()` 在某些 MinGW 實作中無法正確處理 UNC 路徑，會將 `\\server\share\...` 誤判為相對路徑，在前面加上當前工作目錄，產生無效路徑（如 `C:\Windows\System32\\server\share\...`）。

**修復內容**（`utils/zip/unzip.cpp`）：
1. 偵測 UNC 路徑（以 `\\` 開頭），跳過 `absolute()` 轉換
   ```cpp
   if (zip_path_normalized[0] == '\\' && zip_path_normalized[1] == '\\') {
       abs_zip = zip_path_normalized;  // 直接使用，不呼叫 absolute()
   } else {
       abs_zip = std::filesystem::absolute(zip_path);
   }
   ```
2. 確保路徑字串使用反斜線（PowerShell 需要 Windows 格式）
   ```cpp
   std::replace(abs_zip_str.begin(), abs_zip_str.end(), '/', '\\');
   ```
3. 精確化錯誤偵測，避免誤判含有 "Error" 的檔名或路徑
   - 舊：任何含 "Error" 的行都標記為錯誤
   - 新：只標記 PowerShell 特有的錯誤格式（`Exception`、`At line:`、`CategoryInfo` 等）

---

### 上一個修復的 Bug：ZIP 解壓縮失敗後 UI 顯示 FOUND（誤判為成功）

**問題描述**：
ZIP 解壓縮失敗後，Web UI 仍顯示綠色 ✓（FOUND 狀態），讓操作員誤以為 installer 已就緒。

**根本原因**：
1. `copy_worker()` 在開始解壓縮**前**就建立了 `installer/` 資料夾
2. 解壓縮失敗 → 設 `COPY_FAILED`
3. `com_monitor()` 每 2 秒檢查，發現 `installer/` 空資料夾存在 → 覆蓋成 `FOUND`

**修復內容**（`auto_flash/auto_flash.cpp`）：
1. `com_monitor()` 新增條件：不覆蓋 `COPY_FAILED` 狀態
   ```cpp
   if (current_status != FileStatus::COPY_COMPLETE &&
       current_status != FileStatus::COPY_FAILED) {  // ← 新增
       global_file_info.status = FileStatus::FOUND;
   }
   ```
2. `copy_worker()` 解壓縮失敗時清理空資料夾
   ```cpp
   std::filesystem::remove_all(extract_path);
   ```
3. 成功/失敗路徑的 `send_file_status` 移到 lock 外（避免持鎖做網路 I/O）

## 最近完成的工作

### Lock 優化（`LOCK_OPTIMIZATION_COMPLETE.md`）
- `com_mutex` 持有時間從 ~3500ms 降到 ~25ms
- Heartbeat 間隔從 ~2700ms 降到 ~940ms（不再被誤判為 DEAD）
- 主要手法：在鎖內複製資料，鎖外做網路 I/O

### ZIP 解壓縮進度顯示（`ZIP_PROGRESS.md`）
- Windows：PowerShell 腳本輸出 `PROGRESS:` 格式，C++ 解析並回調
- 進度資訊：檔案數、字節數、百分比、速度（MB/s）、ETA
- Web UI 顯示進度條、速度、ETA

## 已知的次要問題（待處理）

0. **ZIP 解壓縮：PowerShell 腳本中的路徑若含單引號會導致語法錯誤**
   - 目前路徑直接嵌入單引號字串：`OpenRead('` + path + `')`
   - 若路徑含 `'` 字元會 break PowerShell 腳本
   - 優先級：低（實際路徑幾乎不含單引號）

1. **`progress_cb` 在 lock 內呼叫 `send_file_status`**
   - 位置：`copy_worker()` 的 ZIP 進度回調
   - 影響：每 5 個檔案做一次網路 I/O，持鎖時間較長
   - 優先級：低（不影響正確性，只影響效能）

2. **`copy_worker()` 早期錯誤路徑在 lock 內呼叫 `send_file_status`**
   - 位置：src/dst 為空、src 不存在的錯誤處理
   - 影響：極少發生，影響小

3. **Linux 版 ZIP 解壓縮無進度回報**
   - 使用 `system("unzip ...")` 無法取得進度
   - 目前 client 只在 Windows 上執行，暫不影響

## 目前 IP 設定

`auto_flash.cpp` 中預設 server IP：
```cpp
std::string ip = "10.235.50.244"; // trueforge-bm
```
可透過命令列參數覆蓋：`auto_flash.exe <server_ip>`