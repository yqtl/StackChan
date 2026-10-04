/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace focus_timer {

enum class Phase : uint8_t { Focus, Break };
enum class State : uint8_t { Ready, Running, Paused, Completed };

constexpr uint32_t kFocusMaximumSeconds = 120U * 60U;
constexpr uint32_t kBreakMaximumSeconds = 30U * 60U;
constexpr uint32_t kExtensionSeconds = 5U * 60U;
constexpr uint64_t kMicrosecondsPerSecond = 1000000ULL;

constexpr uint32_t maximum_seconds(Phase phase)
{
    return phase == Phase::Focus ? kFocusMaximumSeconds : kBreakMaximumSeconds;
}

class TimerModel {
public:
    bool start(Phase phase, uint32_t duration_seconds, uint64_t now_us)
    {
        if (state_ != State::Ready && state_ != State::Completed) {
            return false;
        }

        const uint32_t clamped_seconds =
            std::clamp<uint32_t>(duration_seconds, 1U, maximum_seconds(phase));
        phase_ = phase;
        state_ = State::Running;
        duration_us_ = static_cast<uint64_t>(clamped_seconds) * kMicrosecondsPerSecond;
        remaining_us_ = duration_us_;
        deadline_us_ = add_saturated(now_us, remaining_us_);
        return true;
    }

    bool pause(uint64_t now_us)
    {
        if (state_ != State::Running || now_us >= deadline_us_) {
            return false;
        }
        remaining_us_ = deadline_us_ - now_us;
        state_ = State::Paused;
        return true;
    }

    bool resume(uint64_t now_us)
    {
        if (state_ != State::Paused) {
            return false;
        }
        deadline_us_ = add_saturated(now_us, remaining_us_);
        state_ = State::Running;
        return true;
    }

    // Extending a completed phase starts a fresh five-minute extension.
    // For an active phase, only the remaining time is increased and capped.
    bool extend(uint64_t now_us)
    {
        if (state_ == State::Completed) {
            return start(phase_, kExtensionSeconds, now_us);
        }
        if (state_ != State::Running && state_ != State::Paused) {
            return false;
        }

        const uint64_t current_remaining = remaining_us(now_us);
        if (state_ == State::Running && current_remaining == 0) {
            return false;  // Let update() emit the completion before extending it.
        }
        const uint64_t maximum_remaining =
            static_cast<uint64_t>(maximum_seconds(phase_)) * kMicrosecondsPerSecond;
        const uint64_t accepted = std::min<uint64_t>(
            static_cast<uint64_t>(kExtensionSeconds) * kMicrosecondsPerSecond,
            maximum_remaining - current_remaining);
        if (accepted == 0) {
            return false;
        }

        remaining_us_ = current_remaining + accepted;
        duration_us_ = add_saturated(duration_us_, accepted);
        if (state_ == State::Running) {
            deadline_us_ = add_saturated(now_us, remaining_us_);
        }
        return true;
    }

    // Returns true exactly once when the running deadline expires.
    // A Home cancellation supplied for this update takes precedence.
    bool update(uint64_t now_us, bool cancel_requested = false)
    {
        if (cancel_requested) {
            cancel();
            return false;
        }
        if (state_ != State::Running || now_us < deadline_us_) {
            return false;
        }
        state_ = State::Completed;
        remaining_us_ = 0;
        return true;
    }

    void cancel()
    {
        state_ = State::Ready;
        phase_ = Phase::Focus;
        duration_us_ = 0;
        remaining_us_ = 0;
        deadline_us_ = 0;
    }

    State state() const { return state_; }
    Phase phase() const { return phase_; }
    uint64_t deadline_us() const { return deadline_us_; }
    uint64_t duration_us() const { return duration_us_; }

    uint64_t remaining_us(uint64_t now_us) const
    {
        if (state_ == State::Running) {
            return now_us >= deadline_us_ ? 0 : deadline_us_ - now_us;
        }
        if (state_ == State::Paused) {
            return remaining_us_;
        }
        return 0;
    }

    uint32_t remaining_seconds(uint64_t now_us) const
    {
        const uint64_t remaining = remaining_us(now_us);
        const uint64_t seconds = remaining / kMicrosecondsPerSecond +
                                 (remaining % kMicrosecondsPerSecond == 0 ? 0 : 1);
        return seconds > std::numeric_limits<uint32_t>::max()
                   ? std::numeric_limits<uint32_t>::max()
                   : static_cast<uint32_t>(seconds);
    }

    uint64_t elapsed_us(uint64_t now_us) const
    {
        if (state_ == State::Ready) {
            return 0;
        }
        const uint64_t remaining = remaining_us(now_us);
        return duration_us_ > remaining ? duration_us_ - remaining : 0;
    }

    uint16_t progress_per_mille(uint64_t now_us) const
    {
        if (state_ == State::Completed) {
            return 1000;
        }
        if (duration_us_ == 0) {
            return 0;
        }
        const uint64_t elapsed = std::min(elapsed_us(now_us), duration_us_);
        return static_cast<uint16_t>((elapsed * 1000ULL) / duration_us_);
    }

private:
    static uint64_t add_saturated(uint64_t value, uint64_t increment)
    {
        if (std::numeric_limits<uint64_t>::max() - value < increment) {
            return std::numeric_limits<uint64_t>::max();
        }
        return value + increment;
    }

    State state_ = State::Ready;
    Phase phase_ = Phase::Focus;
    uint64_t deadline_us_ = 0;
    uint64_t duration_us_ = 0;
    uint64_t remaining_us_ = 0;
};

}  // namespace focus_timer
