#pragma once
#include <windows.h>
#include <iostream>
#include <string>
#include <chrono>
#include <unordered_map>
#include <mutex>
#include "common/type.h"
#include "comport/comport.h"

// External references to global variables in auto_flash.cpp
extern std::unordered_map<int, ComportInfo> comport_map;
extern std::mutex com_mutex;

// out_reason（若非 nullptr）在失敗時會被寫入具體原因，供上層回報給 UI。
bool run_emmcdl(const std::string& folder,
                int comport,
                FlashType flash_type,
                std::atomic<bool>& flash_alive,
                Chipset chipset = Chipset::KENAI,
                StorageType storage = StorageType::NVME,
                std::string* out_reason = nullptr);

BOOL flash_image(const std::string& folder,
                 int comport,
                 FlashType flash_type,
                 Chipset chipset = Chipset::KENAI,
                 StorageType storage = StorageType::NVME,
                 std::string* out_reason = nullptr);

void flash_worker(const std::string& folder,
                  int comport,
                  Chipset chipset = Chipset::KENAI,
                  StorageType storage = StorageType::NVME,
                  FlashStage flash_stage = FlashStage::BOTH);