#pragma once

#include <cstdint>

namespace surveillance {

inline constexpr int kYawMin = -1280;
inline constexpr int kYawMax = 1280;
inline constexpr int kSetupPitchMin = 100;
inline constexpr int kSetupPitchMax = 800;
inline constexpr std::uint64_t kScanIntervalMs = 60ULL * 60ULL * 1000ULL;

struct ScanConfig {
    int left_yaw = -450;
    int center_yaw = 0;
    int right_yaw = 450;
    int pitch = 450;
};

struct ServoFeedback {
    int yaw = 0;
    int pitch = 0;
    bool moving = false;
    bool valid = false;
};

enum class ScanStage { Idle, Moving, Settling, Capture, Complete, Failed };
enum class ScanActionKind { None, Move, Capture, Complete, Failed };

struct ScanAction {
    ScanActionKind kind = ScanActionKind::None;
    int yaw = 0;
    int pitch = 0;
    unsigned view_index = 0;
};

class ScanController {
public:
    bool start(std::uint64_t now_ms, const ScanConfig& config)
    {
        if (!valid_config(config)) return false;
        _config = config;
        _step = 0;
        _stage = ScanStage::Moving;
        _movement_started_ms = now_ms;
        _last_progress_ms = now_ms;
        _last_progress_yaw = config.center_yaw;
        _last_progress_pitch = config.pitch;
        _settled_since_ms = 0;
        _capture_announced = false;
        _failure = "";
        return true;
    }

    ScanAction initial_action() const
    {
        return _stage == ScanStage::Moving ? move_action() : ScanAction{};
    }

    ScanAction update(std::uint64_t now_ms, const ServoFeedback& feedback)
    {
        if (_stage == ScanStage::Idle) return {};
        if (_stage == ScanStage::Complete) return {ScanActionKind::Complete};
        if (_stage == ScanStage::Failed) return {ScanActionKind::Failed};
        if (_stage == ScanStage::Capture) return {};
        if (!feedback.valid) return fail("servo feedback invalid");

        const int target = target_yaw();
        if (now_ms - _movement_started_ms >= 5'000) return fail("movement timeout");

        if (absolute(feedback.yaw - _last_progress_yaw) >= 10 ||
            absolute(feedback.pitch - _last_progress_pitch) >= 10) {
            _last_progress_yaw = feedback.yaw;
            _last_progress_pitch = feedback.pitch;
            _last_progress_ms = now_ms;
        }
        if ((feedback.moving || absolute(feedback.yaw - target) > 10) &&
            now_ms - _last_progress_ms >= 1'500) {
            return fail("movement stalled");
        }

        if (absolute(feedback.yaw - target) <= 10 &&
            absolute(feedback.pitch - _config.pitch) <= 10 && !feedback.moving) {
            if (_stage != ScanStage::Settling) {
                _stage = ScanStage::Settling;
                _settled_since_ms = now_ms;
            } else if (now_ms - _settled_since_ms >= 1'000) {
                if (_step >= 3) {
                    _stage = ScanStage::Complete;
                    return {ScanActionKind::Complete};
                }
                _stage = ScanStage::Capture;
                _capture_announced = true;
                return {ScanActionKind::Capture, target, _config.pitch, _step};
            }
        } else {
            _stage = ScanStage::Moving;
            _settled_since_ms = 0;
        }
        return {};
    }

    ScanAction capture_completed(std::uint64_t now_ms, bool saved)
    {
        if (_stage != ScanStage::Capture || !_capture_announced) return {};
        _capture_announced = false;
        if (!saved) return fail("scan image capture failed");
        ++_step;
        _stage = ScanStage::Moving;
        _movement_started_ms = now_ms;
        _last_progress_ms = now_ms;
        _last_progress_yaw = current_target_yaw();
        _last_progress_pitch = _config.pitch;
        _settled_since_ms = 0;
        return move_action();
    }

    ScanAction cancel(const char* reason)
    {
        return fail(reason ? reason : "cancelled");
    }

    ScanStage stage() const { return _stage; }
    const char* failure() const { return _failure; }
    unsigned view_index() const { return _step < 3 ? _step : 3; }
    int watch_yaw() const { return _config.center_yaw; }
    int watch_pitch() const { return _config.pitch; }

    static bool valid_config(const ScanConfig& config)
    {
        return config.left_yaw >= kYawMin && config.left_yaw <= kYawMax &&
               config.center_yaw >= kYawMin && config.center_yaw <= kYawMax &&
               config.right_yaw >= kYawMin && config.right_yaw <= kYawMax &&
               config.left_yaw < config.center_yaw && config.center_yaw < config.right_yaw &&
               config.pitch >= kSetupPitchMin && config.pitch <= kSetupPitchMax;
    }

private:
    static int absolute(int value) { return value < 0 ? -value : value; }

    int current_target_yaw() const
    {
        switch (_step) {
            case 0: return _config.left_yaw;
            case 1: return _config.center_yaw;
            case 2: return _config.right_yaw;
            default: return _config.center_yaw;
        }
    }

    int target_yaw() const { return current_target_yaw(); }

    ScanAction move_action() const
    {
        return {ScanActionKind::Move, target_yaw(), _config.pitch, _step};
    }

    ScanAction fail(const char* reason)
    {
        _stage = ScanStage::Failed;
        _failure = reason;
        return {ScanActionKind::Failed};
    }

    ScanConfig _config{};
    ScanStage _stage = ScanStage::Idle;
    unsigned _step = 0;
    std::uint64_t _movement_started_ms = 0;
    std::uint64_t _last_progress_ms = 0;
    std::uint64_t _settled_since_ms = 0;
    int _last_progress_yaw = 0;
    int _last_progress_pitch = 0;
    bool _capture_announced = false;
    const char* _failure = "";
};

class ScanSchedule {
public:
    void arm(std::uint64_t now_ms)
    {
        _armed = true;
        _next_scan_ms = now_ms;
    }

    void disarm() { _armed = false; }
    bool due(std::uint64_t now_ms) const { return _armed && now_ms >= _next_scan_ms; }
    void completed(std::uint64_t now_ms) { _next_scan_ms = now_ms + kScanIntervalMs; }
    bool armed() const { return _armed; }

private:
    bool _armed = false;
    std::uint64_t _next_scan_ms = 0;
};

inline bool position_drifted(const ServoFeedback& current, int yaw, int pitch)
{
    if (!current.valid || current.moving) return true;
    return current.yaw - yaw > 20 || yaw - current.yaw > 20 ||
           current.pitch - pitch > 20 || pitch - current.pitch > 20;
}

}  // namespace surveillance
