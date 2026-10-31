#pragma once

#define RS485_RX 5
#define RS485_TX 6
#define RS485_DE 7
#define RS485_BAUD 9600

// Number of flap positions on the stock drum. The slave always reports a
// 64-character drum (including the leading blank flap).
#define DRUM_SIZE 64

#define ID_BEGIN 1
#define ID_END 255


// so 40 ms leaves a safe margin. This timeout is paid for every empty ID, so
// it dominates the total scan time (255 IDs x 40 ms ~= 10 s).
#define SCAN_TIMEOUT_MS 40
constexpr unsigned long BUS_RELEASE_MS = 10;