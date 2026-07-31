#pragma once

#include <cmath>
#include <chrono>

#include "../async_sleep.h"
#include "async.h"
#include <iostream>

namespace ouinet::util {

// Sleep for an exponentially increasing duration based on iteration count i.
// The first 3 iterations return immediately (no sleep), then the delay grows
// as 2^(i-3) / 10 seconds, capped at ~12.8 seconds (step clamped to 8).
inline void exponential_backoff(uint32_t i, Async yield) {
    if (i < 3) return;
    uint32_t step = i - 3;
    uint32_t constant_after = 8;
    if (step > constant_after) step = constant_after;
    float delay_s = powf(2, step) / 10.f;
    async_sleep(std::chrono::milliseconds(long(delay_s * 1000.f)), yield);
}

} // namespace ouinet::util
