/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>

// Host-testable guard for the Si12T head-touch baseline drift. After a long uptime the
// sensor can get stuck reporting every zone as touched; recalibrating fixes it, but the
// stale saturated samples must not reach the gesture recogniser afterwards. No ESP-IDF
// or HAL dependencies so the rules can be exercised by the host test suite.
namespace head_touch {

constexpr uint8_t kSaturatedLevel = 2;         // every zone at or above this counts as saturated
constexpr uint32_t kSaturatedRecalMs = 10000;  // a resting palm should not trigger it

class SaturationGuard {
public:
    // Call once per sample. Returns true when the caller should recalibrate now.
    bool should_recalibrate(const uint8_t intensity[3], uint32_t now_ms)
    {
        if (!saturated(intensity)) {
            tracking_ = false;
            return false;
        }
        if (!tracking_) {
            tracking_ = true;
            since_ms_ = now_ms;
            return false;
        }
        // Unsigned subtraction stays correct across tick counter wraparound.
        if (static_cast<uint32_t>(now_ms - since_ms_) >= kSaturatedRecalMs) {
            tracking_ = false;  // a persistent fault re-triggers after another full window
            suppress_ = true;
            return true;
        }
        return false;
    }

    // True while samples must not reach the gesture recogniser: after a recalibration,
    // until one fully untouched sample (all zones 0) is seen. That sample is suppressed too.
    bool suppressing(const uint8_t intensity[3])
    {
        if (!suppress_) {
            return false;
        }
        if (intensity[0] == 0 && intensity[1] == 0 && intensity[2] == 0) {
            suppress_ = false;
        }
        return true;
    }

private:
    static bool saturated(const uint8_t intensity[3])
    {
        return intensity[0] >= kSaturatedLevel && intensity[1] >= kSaturatedLevel &&
               intensity[2] >= kSaturatedLevel;
    }

    bool tracking_     = false;
    bool suppress_     = false;
    uint32_t since_ms_ = 0;
};

}  // namespace head_touch
