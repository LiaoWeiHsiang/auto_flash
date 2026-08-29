# ZIP 解壓縮進度顯示

## 功能說明

當解壓縮 ZIP 文件時，程序會實時顯示：
1. **文件進度**：當前解壓縮第幾個文件 / 總共多少個文件
2. **字節進度**：已解壓縮的字節數 / 總字節數
3. **百分比進度**：解壓縮完成百分比
4. **當前文件名**：正在解壓縮的文件名稱
5. **速度和 ETA**：解壓縮速度（MB/s）和預計剩餘時間

## 日誌輸出範例

### Client 端（auto_flash）
```
[COPY] Detected ZIP file, will extract instead of copy
[UNZIP] Starting to extract: \\server\share\installer.zip
[UNZIP] Destination: C:\installer
[UNZIP] Flatten: yes
[UNZIP] Executing extraction...
[UNZIP] Total files: 245
[UNZIP] Total size: 1234.5 MB
[UNZIP] Progress: 1/245 files (0.5%) - emmcdl.exe
[UNZIP] Progress: 5/245 files (2.3%) - xbl_s_devprg_ns.melf
[UNZIP] Progress: 10/245 files (5.1%) - rawprogram0.xml
[UNZIP] Progress: 15/245 files (8.2%) - WDFlash.xml
...
[UNZIP] Progress: 240/245 files (98.5%) - readme.txt
[UNZIP] Progress: 245/245 files (100.0%) - version.txt
[UNZIP] Extraction completed successfully
[COPY] ZIP extraction completed successfully
```

### Server 端（Web UI）
在 Web UI 上會看到：
```
File Status: COPYING
Progress: 45.2%
Speed: 15.3 MB/s
ETA: 45 seconds
Total: 1234.5 MB / 1234.5 MB
```

## 技術實現

### PowerShell 進度報告

PowerShell 腳本會輸出特殊格式的進度信息：

```powershell
# 開始時輸出總數
TOTAL_FILES:245
TOTAL_BYTES:1294967296

# 每解壓縮一個文件輸出進度
PROGRESS:1:245:5242880:1294967296:emmcdl.exe
PROGRESS:2:245:10485760:1294967296:xbl_s_devprg_ns.melf
...

# 完成時輸出
COMPLETE
```

格式：`PROGRESS:當前文件:總文件:當前字節:總字節:文件名`

### C++ 解析進度

```cpp
// 解析進度行
if (line.find("PROGRESS:") == 0) {
    // PROGRESS:currentFile:totalFiles:currentBytes:totalBytes:filename
    std::string data = line.substr(9);
    
    // 解析各個字段
    current_file = std::stoi(...);
    current_bytes = std::stoull(...);
    filename = ...;
    
    // 計算百分比
    double progress = (double)current_bytes / total_bytes * 100.0;
    
    // 更新 file_info
    global_file_info.progress = progress;
    global_file_info.copied_bytes = current_bytes;
    global_file_info.total_bytes = total_bytes;
    
    // 計算速度和 ETA
    if (elapsed > 0) {
        global_file_info.speed_mbps = mb_copied / elapsed;
        global_file_info.eta_seconds = remaining * elapsed / current_bytes;
    }
}
```

### 進度回調

```cpp
// 定義回調函數
auto progress_cb = [&](int current_file, int total_files, 
                       uint64_t current_bytes, uint64_t total_bytes) {
    // 更新 global_file_info
    global_file_info.progress = (double)current_bytes / total_bytes * 100.0;
    global_file_info.copied_bytes = current_bytes;
    global_file_info.total_bytes = total_bytes;
    
    // 每 5 個文件發送一次狀態更新
    if (current_file % 5 == 0) {
        send_file_status(ip, global_file_info);
    }
};

// 傳遞給 unzip_file
unzip_file(src, dst, true, progress_cb);
```

## 進度更新頻率

### Client 端日誌
- **每個文件**都會輸出進度日誌
- 適合調試和監控

### Server 端更新
- **每 5 個文件**發送一次狀態更新
- 減少網路流量
- 保持 UI 響應性

可以調整更新頻率：
```cpp
// 更頻繁：每個文件都更新
if (current_file % 1 == 0) {
    send_file_status(ip, global_file_info);
}

// 較少更新：每 10 個文件更新
if (current_file % 10 == 0) {
    send_file_status(ip, global_file_info);
}
```

## 性能數據

### 解壓縮速度
- **小文件多**：速度較慢（受文件系統 I/O 限制）
  - 例如：1000 個小文件，平均 5-10 MB/s
- **大文件少**：速度較快
  - 例如：10 個大文件，平均 50-100 MB/s

### 進度更新開銷
- 每次更新：< 1ms（序列化 + 網路發送）
- 每 5 個文件更新：幾乎無影響
- 每個文件都更新：可能增加 5-10% 總時間

## 範例場景

### 場景 1：大型 Installer（2GB，500 個文件）
```
[UNZIP] Total files: 500
[UNZIP] Total size: 2048.0 MB
[UNZIP] Progress: 5/500 files (1.2%) - file1.bin
[UNZIP] Progress: 10/500 files (2.5%) - file2.bin
...
[UNZIP] Progress: 500/500 files (100.0%) - last_file.txt
[UNZIP] Extraction completed successfully

總耗時：約 2-3 分鐘（取決於磁碟速度）
```

### 場景 2：小型 Installer（100MB，50 個文件）
```
[UNZIP] Total files: 50
[UNZIP] Total size: 102.4 MB
[UNZIP] Progress: 5/50 files (10.5%) - emmcdl.exe
[UNZIP] Progress: 10/50 files (22.3%) - config.xml
...
[UNZIP] Progress: 50/50 files (100.0%) - readme.txt
[UNZIP] Extraction completed successfully

總耗時：約 5-10 秒
```

## Web UI 顯示

在 Web UI 上，`file_info` 會實時更新：

```javascript
// 前端輪詢 /clients API
setInterval(() => {
    fetch('/clients')
        .then(res => res.json())
        .then(data => {
            const client = data[0];
            const fileInfo = client.file_info;
            
            // 顯示進度
            if (fileInfo.status === 'copying') {
                progressBar.value = fileInfo.progress;
                statusText.innerText = `Extracting: ${fileInfo.progress.toFixed(1)}%`;
                speedText.innerText = `Speed: ${fileInfo.speed_mbps.toFixed(1)} MB/s`;
                etaText.innerText = `ETA: ${fileInfo.eta_seconds}s`;
            }
        });
}, 1000);  // 每秒更新一次
```

## 錯誤處理

### 解壓縮失敗
```
[UNZIP] Progress: 123/500 files (45.2%) - corrupted_file.bin
[UNZIP ERROR] PowerShell exit code: 1
[COPY ERROR] ZIP extraction failed

file_info.status = COPY_FAILED
file_info.error_msg = "ZIP extraction failed"
```

### 磁碟空間不足
```
[UNZIP] Progress: 234/500 files (78.5%) - large_file.bin
[UNZIP ERROR] Failed to extract: Disk full
[COPY ERROR] ZIP extraction failed

file_info.status = COPY_FAILED
file_info.error_msg = "ZIP extraction failed"
```

## 調試技巧

### 查看詳細進度
所有進度信息都會輸出到 stdout，可以重定向到文件：
```bash
auto_flash.exe > extraction.log 2>&1
```

### 測試不同大小的 ZIP
```bash
# 小 ZIP（快速測試）
auto_flash.exe test_small.zip

# 大 ZIP（壓力測試）
auto_flash.exe test_large.zip
```

## 未來改進

1. **更精確的進度**：
   - 當前：基於文件數量和字節數
   - 改進：基於實際解壓縮的字節數（需要更複雜的實現）

2. **暫停/恢復**：
   - 支持暫停解壓縮
   - 支持從中斷處恢復

3. **並行解壓縮**：
   - 使用多線程加速解壓縮
   - 適合多核 CPU

4. **壓縮比顯示**：
   - 顯示壓縮前後的大小對比
   - 顯示壓縮比（例如：50%）
