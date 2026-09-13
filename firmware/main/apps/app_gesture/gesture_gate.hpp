#pragma once
#include <cstdint>

// Only confirmed, single-hand observations may advance the hold timer.
class GestureGate {
public:
    static constexpr int64_t hold_ms = 1000;
    static constexpr int64_t release_ms = 1000;
    // Detector + classifier takes about 830 ms on StackChan. Allow normal
    // frame jitter while still resetting the hold after a stalled worker.
    static constexpr int64_t maximum_gap_ms = 1200;

    bool update(int64_t now, bool thumbs_up, bool no_hand) {
        if (last_ >= 0 && (now < last_ || now - last_ > maximum_gap_ms)) {
            hold_start_ = release_start_ = -1;
        }
        last_ = now;
        if (latched_) {
            if (!no_hand) {
                release_start_ = -1;
            } else {
                if (release_start_ < 0) release_start_ = now;
                if (now - release_start_ >= release_ms) {
                    latched_ = false;
                    release_start_ = -1;
                }
            }
            return false;
        }
        if (!thumbs_up) {
            hold_start_ = -1;
            return false;
        }
        if (hold_start_ < 0) hold_start_ = now;
        if (now - hold_start_ < hold_ms) return false;
        latched_ = true;
        hold_start_ = -1;
        return true;
    }

    bool latched() const { return latched_; }

private:
    int64_t last_ = -1;
    int64_t hold_start_ = -1;
    int64_t release_start_ = -1;
    bool latched_ = false;
};
