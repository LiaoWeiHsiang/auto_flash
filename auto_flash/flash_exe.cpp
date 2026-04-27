#include <windows.h>
#include <iostream>
#include <string>
#include <chrono>
#include "common/type.h"
#include "comport/comport.h"
#include "flash/flash.h"

int main(int argc, char* argv[])
{
    printf("Start\n");

    int comport = -1;
    FlashType flash_type = FlashType::NONE; // 預設值

    if (argc > 2) {
        for (int i = 1; i < argc; ++i) {

            std::string arg = argv[i];

            if (arg == "--comport" && i + 1 < argc) {
                comport = std::stoi(argv[++i]);   // 吃掉下一個參數
            }
            else if (arg == "--flashtype" && i + 1 < argc) {
                std::string type = argv[++i];
                if (type == "spinor") {
                    flash_type = FlashType::SPINOR;
                }
                else if (type == "hlos") {
                    flash_type = FlashType::HLOS;
                }
                else {
                    std::cerr << "Unknown flashtype: " << type << "\n";
                    return 1;
                }
            }
        }
    }

    if (comport < 0) {
        std::cerr << "Usage:\n"
                  << "  auto_flash.exe --comport <num> [--flashtype spinor|hlos]\n";
        return 1;
    }

    std::cout << "COM port = " << comport << "\n";
    std::cout << "Flash type = "
              << (flash_type == FlashType::SPINOR ? "SPINOR" : (flash_type == FlashType::HLOS ? "HLOS" : "NONE"))              << "\n";


    std::string folder = "C:\\workspace\\Glymur\\r4900\\Installer";

    if (flash_type == FlashType::NONE || flash_type == FlashType::SPINOR){
        
        
        if(flash_image(folder, comport, FlashType::SPINOR) != 0){
            std::cerr << "\nFlash spinor FAILED\n";
            return 1;
        }else{
            std::cout << "\nFlash spinor SUCCESS\n";
        }
    }

    Sleep(1 * 1000);  // 1s

    if (flash_type == FlashType::NONE || flash_type == FlashType::HLOS){

        if(flash_image(folder, comport, FlashType::HLOS) != 0){
            std::cerr << "Flash HLOS FAILED\n";
            return 1;
        }else{
            std::cout << "Flash HLOS SUCCESS\n";
        }
    }

    // for (int attempt = 1; attempt <= MAX_RETRY; ++attempt)
    // {
    //     std::cout << "\n Attempt " << attempt << "/" << MAX_RETRY << "\n";

    //     if (run_emmcdl("C:\\workspace\\Glymur\\r4900\\Installer", comport))
    //     {
    //         std::cout << "\n Flash SUCCESS\n";
    //         return 0;
    //     }

    //     std::cout << "\n Retry...\n";
    // }

    
    // std::cout << "Press Ctrl+C to exit, sleeping for 10 seconds...\n";
    // Sleep(10 * 1000);  // 10 秒

    return 1;
}