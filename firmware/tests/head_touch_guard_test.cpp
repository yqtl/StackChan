#include "head_touch_guard.hpp"

#include <cassert>
#include <cstdint>
#include <initializer_list>

using namespace head_touch;

namespace {

constexpr uint8_t kAll3[3]      = {3, 3, 3};
constexpr uint8_t kPartial[3]   = {3, 1, 3};
constexpr uint8_t kFlicker[3]   = {3, 2, 3};
constexpr uint8_t kUntouched[3] = {0, 0, 0};
constexpr uint8_t kLight[3]     = {0, 1, 0};

// Feeds the same sample every 50 ms from start_ms. Returns the elapsed time at which the
// guard asked for a recalibration, or UINT32_MAX if it never did within max_ms.
uint32_t feed_until_recal(SaturationGuard& guard, const uint8_t intensity[3], uint32_t start_ms, uint32_t max_ms)
{
    for (uint32_t elapsed = 0; elapsed <= max_ms; elapsed += 50) {
        if (guard.should_recalibrate(intensity, start_ms + elapsed)) {
            return elapsed;
        }
    }
    return UINT32_MAX;
}

}  // namespace

int main()
{
    {
        // (a) Saturated for 9999 ms does not recalibrate; 10000 ms does.
        SaturationGuard guard;
        assert(!guard.should_recalibrate(kAll3, 1000));
        assert(!guard.should_recalibrate(kAll3, 1000 + kSaturatedRecalMs - 1));
        assert(guard.should_recalibrate(kAll3, 1000 + kSaturatedRecalMs));
    }

    {
        // (a) A first saturated sample at time 0 is tracked like any other (no 0 sentinel).
        SaturationGuard guard;
        assert(!guard.should_recalibrate(kAll3, 0));
        assert(!guard.should_recalibrate(kAll3, kSaturatedRecalMs - 1));
        assert(guard.should_recalibrate(kAll3, kSaturatedRecalMs));
    }

    {
        // (b) One non-saturated sample resets the window.
        SaturationGuard guard;
        assert(!guard.should_recalibrate(kAll3, 0));
        assert(!guard.should_recalibrate(kAll3, 9000));
        assert(!guard.should_recalibrate(kPartial, 9050));
        assert(!guard.should_recalibrate(kAll3, 9100));
        assert(!guard.should_recalibrate(kAll3, 9100 + kSaturatedRecalMs - 1));
        assert(guard.should_recalibrate(kAll3, 9100 + kSaturatedRecalMs));
    }

    {
        // (c) After a recalibration, saturated and partial samples stay suppressed until
        // one fully untouched sample has been seen (and that sample is suppressed too).
        SaturationGuard guard;
        assert(!guard.suppressing(kAll3));  // Nothing suppressed before any recalibration.
        guard.should_recalibrate(kAll3, 0);
        assert(guard.should_recalibrate(kAll3, kSaturatedRecalMs));
        assert(guard.suppressing(kAll3));
        assert(guard.suppressing(kFlicker));
        assert(guard.suppressing(kPartial));
        assert(guard.suppressing(kLight));
        assert(guard.suppressing(kUntouched));  // The clearing sample itself is suppressed.
        assert(!guard.suppressing(kUntouched));
        assert(!guard.suppressing(kLight));
        assert(!guard.suppressing(kAll3));
    }

    {
        // (d) A persistent fault re-triggers one full window after the previous recalibration.
        SaturationGuard guard;
        uint32_t t = 5000;
        assert(feed_until_recal(guard, kAll3, t, kSaturatedRecalMs) == kSaturatedRecalMs);
        t += kSaturatedRecalMs;
        assert(guard.suppressing(kAll3));
        // Immediately after the recalibration it must not fire again.
        assert(!guard.should_recalibrate(kAll3, t + 50));
        assert(!guard.should_recalibrate(kAll3, t + 50 + kSaturatedRecalMs - 1));
        assert(guard.should_recalibrate(kAll3, t + 50 + kSaturatedRecalMs));
        // Suppression is independent of the saturation counting and stays on.
        assert(guard.suppressing(kAll3));
    }

    {
        // (e) A 3 2 3 / 3 3 3 flicker still counts as saturated.
        SaturationGuard guard;
        bool fired = false;
        uint32_t fired_at = 0;
        for (uint32_t t = 0; t <= kSaturatedRecalMs && !fired; t += 50) {
            const uint8_t* sample = ((t / 50) % 2 == 0) ? kAll3 : kFlicker;
            if (guard.should_recalibrate(sample, t)) {
                fired    = true;
                fired_at = t;
            }
        }
        assert(fired);
        assert(fired_at == kSaturatedRecalMs);
    }

    {
        // (f) Tick wraparound: the window started just before UINT32_MAX.
        SaturationGuard guard;
        const uint32_t start = UINT32_MAX - 4000;
        assert(!guard.should_recalibrate(kAll3, start));
        assert(!guard.should_recalibrate(kAll3, start + 5000));  // Wrapped to 995.
        assert(!guard.should_recalibrate(kAll3, start + kSaturatedRecalMs - 1));
        assert(guard.should_recalibrate(kAll3, start + kSaturatedRecalMs));

        // Same with the wrap exactly on the first sample.
        SaturationGuard exact;
        assert(!exact.should_recalibrate(kAll3, UINT32_MAX));
        assert(!exact.should_recalibrate(kAll3, UINT32_MAX + kSaturatedRecalMs - 1));
        assert(exact.should_recalibrate(kAll3, UINT32_MAX + kSaturatedRecalMs));
    }

    {
        // (g) Normal short touches never recalibrate and are never suppressed.
        SaturationGuard guard;
        uint32_t t = 0;
        for (int tap = 0; tap < 100; ++tap) {
            for (const uint8_t* sample : {kLight, kPartial, kFlicker, kAll3, kAll3, kPartial, kUntouched}) {
                assert(!guard.should_recalibrate(sample, t));
                assert(!guard.suppressing(sample));
                t += 50;
            }
            t += 2000;  // Idle gap between touches.
        }
        // A long, firm all-zone press just under the window is also left alone.
        assert(!guard.should_recalibrate(kAll3, t));
        assert(!guard.should_recalibrate(kAll3, t + kSaturatedRecalMs - 1));
        assert(!guard.should_recalibrate(kUntouched, t + kSaturatedRecalMs));
        assert(!guard.suppressing(kUntouched));
    }

    return 0;
}
