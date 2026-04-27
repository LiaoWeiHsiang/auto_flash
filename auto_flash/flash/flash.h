#pragma once
#include <windows.h>
#include <iostream>
#include <string>
#include <chrono>
#include "common/type.h"
#include "comport/comport.h"


bool run_emmcdl(const std::string& folder, int comport, FlashType flash_type);
BOOL flash_image(const std::string& folder, int comport, FlashType flash_type);
void flash_worker(const std::string& folder, int comport);