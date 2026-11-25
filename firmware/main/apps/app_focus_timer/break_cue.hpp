/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <cstdint>

namespace focus_timer {

// Physical "time for a break" exchange shown when a focus phase completes:
// StackChan faces the user, nods N times (blue LED), then waits for N head
// taps (amber LED, each tap flashes white and bobs the head). A match starts
// the break (green LED, hop); a mismatch shakes the head (red LED) and replays.
// The cue only produces commands, so the timing is host-testable.
enum class CueStage : uint8_t { Idle, Turning, Nodding, Listening, Missed, Matched, Resting };

struct CueCommand {
    bool move = false;
    int16_t yaw = 0;    // 0.1 degree units, as used by Motion
    int16_t pitch = 0;  // 0.1 degree units, 0 is level and positive is up
    int16_t speed = 0;  // Motion speed, 0-1000
    bool light = false;
    uint32_t rgb = 0;
    bool start_break = false;
};

constexpr int16_t kCueFaceYaw = 0;
constexpr int16_t kCueFacePitch = 200;
constexpr int16_t kCueNodPitch = 50;
constexpr int16_t kCueShakeYaw = 120;
constexpr int16_t kCueHopPitch = 350;
constexpr int16_t kCueTapBobPitch = 150;
constexpr int16_t kCueTurnSpeed = 300;
constexpr int16_t kCueNodSpeed = 700;
constexpr int16_t kCueHomeSpeed = 200;

constexpr uint32_t kCueOffColor = 0x000000;
constexpr uint32_t kCueBreakColor = 0x2F6BFF;   // demo: watch the nods
constexpr uint32_t kCueListenColor = 0xFFB300;  // your turn: tap
constexpr uint32_t kCueTapColor = 0xFFFFFF;     // a tap was received
constexpr uint32_t kCueMissColor = 0xFF2D2D;    // reject
constexpr uint32_t kCueMatchColor = 0x2EE86B;   // accept
constexpr uint32_t kCueRestColor = kCueOffColor;

constexpr uint8_t kCueMinimumNods = 1;
constexpr uint8_t kCueMaximumNods = 4;
constexpr uint8_t kCueMaximumCalls = 3;

constexpr uint64_t kCueTurnMinimumUs = 400000ULL;
constexpr uint64_t kCueTurnMaximumUs = 2500000ULL;
constexpr uint64_t kCueNodHalfUs = 350000ULL;
constexpr uint64_t kCueAfterNodsUs = 300000ULL;
constexpr uint64_t kCueTapFlashUs = 180000ULL;
constexpr uint64_t kCueTapSettleUs = 1000000ULL;
constexpr uint64_t kCueListenTimeoutUs = 8000000ULL;
constexpr uint64_t kCueShakeStepUs = 300000ULL;
constexpr uint64_t kCueMissUs = 1100000ULL;
constexpr uint64_t kCueHopUs = 300000ULL;
constexpr uint64_t kCueMatchUs = 800000ULL;
constexpr uint64_t kCueRecallUs = 45000000ULL;

class BreakCue {
public:
    CueCommand begin(uint8_t nods, uint64_t now_us)
    {
        expected_ = std::clamp(nods, kCueMinimumNods, kCueMaximumNods);
        calls_ = 0;
        return enter_turning(now_us);
    }

    void cancel()
    {
        stage_ = CueStage::Idle;
        taps_ = 0;
    }

    // Head-touch presses. Only a reply after the nods counts; while resting,
    // a tap asks StackChan to show the rhythm again.
    CueCommand on_tap(uint64_t now_us)
    {
        if (stage_ == CueStage::Listening) {
            if (taps_ < UINT8_MAX) {
                ++taps_;
            }
            ++tap_serial_;
            last_tap_us_ = now_us;
            tap_flash_ = true;
            CueCommand command = move_to(kCueFaceYaw, kCueTapBobPitch, kCueNodSpeed);
            command.light = true;
            command.rgb = kCueTapColor;
            return command;
        } else if (stage_ == CueStage::Resting) {
            return enter_turning(now_us);
        }
        return {};
    }

    CueCommand update(uint64_t now_us, bool motion_busy)
    {
        const uint64_t elapsed = now_us > stage_start_us_ ? now_us - stage_start_us_ : 0;
        switch (stage_) {
            case CueStage::Idle:
                return {};
            case CueStage::Turning:
                if (elapsed >= kCueTurnMaximumUs || (elapsed >= kCueTurnMinimumUs && !motion_busy)) {
                    return enter_nodding(now_us);
                }
                return {};
            case CueStage::Nodding: {
                const uint64_t nodding_us = static_cast<uint64_t>(expected_) * 2U * kCueNodHalfUs;
                if (elapsed >= nodding_us + kCueAfterNodsUs) {
                    stage_ = CueStage::Listening;
                    stage_start_us_ = now_us;
                    taps_ = 0;
                    tap_flash_ = false;
                    CueCommand command;
                    command.light = true;
                    command.rgb = kCueListenColor;
                    return command;
                }
                const uint32_t step = static_cast<uint32_t>(std::min(elapsed, nodding_us) / kCueNodHalfUs);
                if (step == step_) {
                    return {};
                }
                step_ = step;
                const bool down = step % 2 == 0 && elapsed < nodding_us;
                return move_to(kCueFaceYaw, down ? kCueNodPitch : kCueFacePitch, kCueNodSpeed);
            }
            case CueStage::Listening:
                if (tap_flash_ && now_us - last_tap_us_ >= kCueTapFlashUs) {
                    tap_flash_ = false;
                    CueCommand command = move_to(kCueFaceYaw, kCueFacePitch, kCueNodSpeed);
                    command.light = true;
                    command.rgb = kCueListenColor;
                    return command;
                }
                if (taps_ > 0 && now_us - last_tap_us_ >= kCueTapSettleUs) {
                    return taps_ == expected_ ? enter_matched(now_us) : enter_missed(now_us);
                }
                if (taps_ == 0 && elapsed >= kCueListenTimeoutUs) {
                    return enter_resting(now_us);
                }
                return {};
            case CueStage::Missed: {
                if (elapsed >= kCueMissUs) {
                    return enter_nodding(now_us);
                }
                const uint32_t step = static_cast<uint32_t>(elapsed / kCueShakeStepUs);
                if (step == step_) {
                    return {};
                }
                step_ = step;
                const int16_t yaw = step == 0 ? -kCueShakeYaw : step == 1 ? kCueShakeYaw : kCueFaceYaw;
                return move_to(yaw, kCueFacePitch, kCueNodSpeed);
            }
            case CueStage::Matched:
                if (elapsed >= kCueMatchUs) {
                    stage_ = CueStage::Idle;
                    CueCommand command = move_to(0, 0, kCueHomeSpeed);
                    command.light = true;
                    command.rgb = kCueOffColor;
                    return command;
                }
                if (elapsed >= kCueHopUs && step_ == 0) {
                    step_ = 1;
                    return move_to(kCueFaceYaw, kCueFacePitch, kCueNodSpeed);
                }
                return {};
            case CueStage::Resting:
                if (calls_ < kCueMaximumCalls && elapsed >= kCueRecallUs) {
                    return enter_turning(now_us);
                }
                return {};
        }
        return {};
    }

    bool active() const { return stage_ != CueStage::Idle; }
    // Matched keeps running briefly after the break starts, for the celebration.
    bool celebrating() const { return stage_ == CueStage::Matched; }
    CueStage stage() const { return stage_; }
    uint8_t expected() const { return expected_; }
    uint8_t taps() const { return taps_; }
    uint8_t calls() const { return calls_; }
    // Counts every accepted tap, so a view can animate each one.
    uint32_t tap_serial() const { return tap_serial_; }

private:
    static CueCommand move_to(int16_t yaw, int16_t pitch, int16_t speed)
    {
        CueCommand command;
        command.move = true;
        command.yaw = yaw;
        command.pitch = pitch;
        command.speed = speed;
        return command;
    }

    CueCommand enter_turning(uint64_t now_us)
    {
        stage_ = CueStage::Turning;
        stage_start_us_ = now_us;
        taps_ = 0;
        if (calls_ < UINT8_MAX) {
            ++calls_;
        }
        CueCommand command = move_to(kCueFaceYaw, kCueFacePitch, kCueTurnSpeed);
        command.light = true;
        command.rgb = kCueBreakColor;
        return command;
    }

    CueCommand enter_nodding(uint64_t now_us)
    {
        stage_ = CueStage::Nodding;
        stage_start_us_ = now_us;
        step_ = 0;
        taps_ = 0;
        CueCommand command = move_to(kCueFaceYaw, kCueNodPitch, kCueNodSpeed);
        command.light = true;
        command.rgb = kCueBreakColor;
        return command;
    }

    CueCommand enter_missed(uint64_t now_us)
    {
        stage_ = CueStage::Missed;
        stage_start_us_ = now_us;
        step_ = 0;
        CueCommand command = move_to(-kCueShakeYaw, kCueFacePitch, kCueNodSpeed);
        command.light = true;
        command.rgb = kCueMissColor;
        return command;
    }

    CueCommand enter_matched(uint64_t now_us)
    {
        stage_ = CueStage::Matched;
        stage_start_us_ = now_us;
        step_ = 0;
        CueCommand command = move_to(kCueFaceYaw, kCueHopPitch, kCueNodSpeed);
        command.light = true;
        command.rgb = kCueMatchColor;
        command.start_break = true;
        return command;
    }

    CueCommand enter_resting(uint64_t now_us)
    {
        stage_ = CueStage::Resting;
        stage_start_us_ = now_us;
        CueCommand command = move_to(0, 0, kCueHomeSpeed);
        command.light = true;
        command.rgb = kCueRestColor;
        return command;
    }

    CueStage stage_ = CueStage::Idle;
    uint64_t stage_start_us_ = 0;
    uint64_t last_tap_us_ = 0;
    uint32_t step_ = 0;
    uint32_t tap_serial_ = 0;
    bool tap_flash_ = false;
    uint8_t expected_ = 2;
    uint8_t taps_ = 0;
    uint8_t calls_ = 0;
};

}  // namespace focus_timer
