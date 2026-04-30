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

bool run_emmcdl(const std::string& folder, int comport, FlashType flash_type);
BOOL flash_image(const std::string& folder, int comport, FlashType flash_type);
void flash_worker(const std::string& folder, int comport);