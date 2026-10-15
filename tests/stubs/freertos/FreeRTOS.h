#pragma once

#include <chrono>
#include <cstdint>

using BaseType_t = int;
using TickType_t = uint32_t;

#define pdFALSE 0
#define pdTRUE 1
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(ms) (static_cast<TickType_t>(ms))
#define BIT0 0x00000001
#define BIT1 0x00000002

inline TickType_t xTaskGetTickCount() {
    using namespace std::chrono;
    return static_cast<TickType_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
