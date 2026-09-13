#pragma once
#include <cstdint>

// Only confirmed, single-hand observations may advance the hold timer.
class GestureGate {
public:
    static constexpr int64_t hold_ms = 250;
    static constexpr unsigned minimum_observations = 2;
    static constexpr int64_t release_ms = 1000;
    // Also tolerate the previous single-core cadence (~830 ms), but never
    // count a stalled worker as evidence that a pose was held or removed.
    static constexpr int64_t maximum_gap_ms = 1200;

    bool update(int64_t now, bool thumbs_up, bool no_hand) {
        if (last_ >= 0 && (now < last_ || now - last_ > maximum_gap_ms)) {
            hold_start_ = release_start_ = -1;
            observations_ = 0;
        }
        if (now == last_) return false; // Repeated timestamps are not fresh frames.
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
            observations_ = 0;
            return false;
        }
        if (hold_start_ < 0) hold_start_ = now;
        if (observations_ < minimum_observations) ++observations_;
        if (observations_ < minimum_observations || now - hold_start_ < hold_ms) return false;
        latched_ = true;
        hold_start_ = -1;
        observations_ = 0;
        return true;
    }

    bool latched() const { return latched_; }

private:
    int64_t last_ = -1;
    int64_t hold_start_ = -1;
    int64_t release_start_ = -1;
    unsigned observations_ = 0;
    bool latched_ = false;
};
