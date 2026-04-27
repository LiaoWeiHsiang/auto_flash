#include "comport/comport.h"

bool get_comport_9008(int& out_com)
{
    HDEVINFO hDevInfo = SetupDiGetClassDevs(
        &GUID_DEVCLASS_PORTS,
        NULL,
        NULL,
        DIGCF_PRESENT
    );

    if (hDevInfo == INVALID_HANDLE_VALUE)
        return false;

    SP_DEVINFO_DATA devInfoData;
    devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

    char buffer[256];

    for (DWORD i = 0;
         SetupDiEnumDeviceInfo(hDevInfo, i, &devInfoData);
         i++)
    {
        if (SetupDiGetDeviceRegistryPropertyA(
                hDevInfo,
                &devInfoData,
                SPDRP_FRIENDLYNAME,
                NULL,
                (PBYTE)buffer,
                sizeof(buffer),
                NULL))
        {
            std::string name(buffer);
            std::cout << "device name = " << name << std::endl;
            // filter device name contains "9008"
            if (name.find("9008") != std::string::npos)
            {
                // extract COM number
                std::regex r("COM(\\d+)");
                std::smatch match;

                if (std::regex_search(name, match, r))
                {
                    out_com = std::stoi(match[1]);
                    SetupDiDestroyDeviceInfoList(hDevInfo);
                    return true;
                }
            }
        }
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
    return false;
}



bool get_all_9008_comports(std::vector<int>& out_coms)
{
    HDEVINFO hDevInfo = SetupDiGetClassDevs(
        &GUID_DEVCLASS_PORTS,
        NULL,
        NULL,
        DIGCF_PRESENT
    );

    if (hDevInfo == INVALID_HANDLE_VALUE)
        return false;

    SP_DEVINFO_DATA devInfoData;
    devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

    char buffer[256];

    for (DWORD i = 0;
         SetupDiEnumDeviceInfo(hDevInfo, i, &devInfoData);
         i++)
    {
        if (SetupDiGetDeviceRegistryPropertyA(
                hDevInfo,
                &devInfoData,
                SPDRP_FRIENDLYNAME,
                NULL,
                (PBYTE)buffer,
                sizeof(buffer),
                NULL))
        {
            std::string name(buffer);

            if (name.find("9008") != std::string::npos)
            {
                std::regex r("COM(\\d+)");
                std::smatch match;

                if (std::regex_search(name, match, r))
                {
                    int com = std::stoi(match[1]);
                    out_coms.push_back(com);

                    // std::cout << "[FOUND] " << name << std::endl;
                }
            }
        }
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
    return !out_coms.empty();
}