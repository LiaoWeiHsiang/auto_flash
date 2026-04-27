#pragma once
#include <stdint.h>

enum MsgType {
    MSG_HEART_BEAT,
    MSG_AUTO_FLASH_STATUS,
    MSG_COMPORT,
    MSG_PROGRESS,
    MSG_LOG,
    MSG_SET_AUTO_FLASH
};

struct Packet {
    uint8_t type;
    uint32_t length;
    char data[];
};

enum class FlashType {
    NONE,
    SPINOR,
    HLOS
};