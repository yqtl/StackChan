#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace surveillance {

inline constexpr std::size_t kMotionWidth = 80;
inline constexpr std::size_t kMotionHeight = 60;
inline constexpr std::size_t kMotionPixels = kMotionWidth * kMotionHeight;
inline constexpr std::uint64_t kBurstCooldownMs = 10'000;

enum class MotionResult { Baseline, Quiet, Detected, Suspended };

class MotionDetector {
public:
    explicit MotionDetector(unsigned changed_percent = 3, unsigned pixel_delta = 20)
        : _changed_percent(changed_percent), _pixel_delta(pixel_delta)
    {
    }

    void set_sensitivity_level(unsigned level)
    {
        const unsigned changed_percent = level >= 3 ? 1 : level <= 1 ? 6 : 3;
        if (changed_percent != _changed_percent) {
            _changed_percent = changed_percent;
            _consecutive_hits = 0;
            _has_baseline = false;
        }
    }

    MotionResult process(const std::uint8_t* pixels, std::size_t size,
                         std::uint64_t now_ms, bool enabled = true)
    {
        if (!enabled) {
            reset();
            return MotionResult::Suspended;
        }
        if (!pixels || size != kMotionPixels) {
            reset();
            return MotionResult::Quiet;
        }
        if (!_has_baseline) {
            copy_baseline(pixels);
            return MotionResult::Baseline;
        }

        std::int32_t total_delta = 0;
        for (std::size_t i = 0; i < kMotionPixels; ++i) {
            total_delta += static_cast<int>(pixels[i]) - static_cast<int>(_baseline[i]);
        }
        const int brightness_shift = total_delta / static_cast<int>(kMotionPixels);
        std::size_t changed = 0;
        for (std::size_t i = 0; i < kMotionPixels; ++i) {
            const int corrected = static_cast<int>(pixels[i]) -
                                  static_cast<int>(_baseline[i]) - brightness_shift;
            if (absolute(corrected) >= static_cast<int>(_pixel_delta)) ++changed;
        }

        if (changed * 100 > kMotionPixels * _changed_percent) {
            if (_consecutive_hits < 2) ++_consecutive_hits;
        } else {
            _consecutive_hits = 0;
            // Rebase on quiet frames so slow lighting changes do not accumulate.
            copy_baseline(pixels);
        }

        if (_consecutive_hits >= 2 && now_ms >= _next_allowed_ms) {
            _next_allowed_ms = now_ms + kBurstCooldownMs;
            copy_baseline(pixels);
            return MotionResult::Detected;
        }
        return MotionResult::Quiet;
    }

    void reset()
    {
        _has_baseline = false;
        _consecutive_hits = 0;
    }

private:
    static int absolute(int value) { return value < 0 ? -value : value; }

    void copy_baseline(const std::uint8_t* pixels)
    {
        for (std::size_t i = 0; i < kMotionPixels; ++i) _baseline[i] = pixels[i];
        _has_baseline = true;
    }

    std::array<std::uint8_t, kMotionPixels> _baseline{};
    unsigned _changed_percent;
    unsigned _pixel_delta;
    std::uint64_t _next_allowed_ms = 0;
    unsigned _consecutive_hits = 0;
    bool _has_baseline = false;
};

}  // namespace surveillance
