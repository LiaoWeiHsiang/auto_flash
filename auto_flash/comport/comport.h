#pragma once

#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <regstr.h>

#include <thread>
#include <chrono>
#include <iostream>
#include <regex>
#include <string>

// #pragma comment(lib, "setupapi.lib")

bool get_comport_9008(int& out_com);
bool get_all_9008_comports(std::vector<int>& out_coms);