/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_focus_timer.h"

#include <apps/common/home_indicator/home_indicator.h>
#include <apps/common/status_bar/status_bar.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <hal/hal.h>
#include <settings.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>

namespace {

constexpr const char* TAG = "FOCUS";
constexpr const char* kSettingsNamespace = "focus_timer";
constexpr uint64_t kAnimationFrameUs = 100000ULL;
constexpr uint64_t kWalkAndStretchCycleUs = 3000000ULL;
constexpr uint64_t kCompletionAnimationUs = 3000000ULL;

constexpr uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue)
{
    return static_cast<uint16_t>(((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3));
}

struct ClockIcon {
    static constexpr uint16_t kWidth = 24;
    static constexpr uint16_t kHeight = 24;
    std::array<uint16_t, kWidth * kHeight> pixels{};
    lv_image_dsc_t image{};

    ClockIcon()
    {
        constexpr uint16_t background = rgb565(8, 59, 76);
        constexpr uint16_t ring = rgb565(255, 209, 102);
        constexpr uint16_t hands = rgb565(217, 243, 240);
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const int dx = x - 11;
                const int dy = y - 11;
                const int distance_squared = dx * dx + dy * dy;
                uint16_t color = background;
                if (distance_squared >= 49 && distance_squared <= 81) {
                    color = ring;
                }
                if ((x == 11 && y >= 5 && y <= 11) || (y == 11 && x >= 11 && x <= 17)) {
                    color = hands;
                }
                pixels[static_cast<std::size_t>(y * kWidth + x)] = color;
            }
        }

        image.header.magic = LV_IMAGE_HEADER_MAGIC;
        image.header.cf = LV_COLOR_FORMAT_RGB565;
        image.header.flags = 0;
        image.header.w = kWidth;
        image.header.h = kHeight;
        image.header.stride = kWidth * sizeof(uint16_t);
        image.header.reserved_2 = 0;
        image.data_size = sizeof(pixels);
        image.data = reinterpret_cast<const uint8_t*>(pixels.data());
    }
};

uint64_t monotonic_us()
{
    return static_cast<uint64_t>(esp_timer_get_time());
}

}  // namespace

AppFocusTimer::AppFocusTimer()
{
    setAppInfo().name = "FOCUS";
    static const ClockIcon clock_icon;
    setAppInfo().icon = const_cast<lv_image_dsc_t*>(&clock_icon.image);
    static uint32_t theme_color = 0xFFD166;
    setAppInfo().userData = &theme_color;
}

AppFocusTimer::~AppFocusTimer() = default;

void AppFocusTimer::onCreate()
{
    ESP_LOGI(TAG, "on create");
}

void AppFocusTimer::onOpen()
{
    load_preferences();
    _timer.cancel();
    _pending_action.store(focus_timer::Action::None);
    _home_requested.store(false);
    _home_closing = false;
    _completion_animation_start_us = 0;
    _last_animation_frame_us = 0;
    _current_pose = focus_timer::Pose::Idle;

    LvglLockGuard lock;
    _view = std::make_unique<focus_timer::FocusTimerView>(
        [this](focus_timer::Action action) { queue_action(action); });
    view::create_home_indicator([this]() { _home_requested.store(true); }, 0xFFD166, 0x083B4C);
    view::create_status_bar(0xFFD166, 0x083B4C);
    _view->update(make_view_state(monotonic_us()));
    ESP_LOGI(TAG, "ready");
}

void AppFocusTimer::onRunning()
{
    {
        LvglLockGuard lock;
        view::update_home_indicator();
        view::update_status_bar();
    }

    if (_home_requested.exchange(false)) {
        begin_home_close();
    }

    FocusChime::Result chime_result = FocusChime::Result::None;
    if (_chime.poll_finished(&chime_result) && chime_result == FocusChime::Result::Failed &&
        !_audio_failure_logged) {
        ESP_LOGW(TAG, "completion chime could not play; visual completion remains available");
        _audio_failure_logged = true;
    }

    if (_home_closing) {
        if (!_chime.active()) {
            close();
        }
        return;
    }

    const uint64_t now_us = monotonic_us();
    const bool home_cancelled = _home_requested.exchange(false);
    if (home_cancelled) {
        begin_home_close();
    }
    const bool completed_now = _timer.update(now_us, home_cancelled);
    if (home_cancelled || _home_closing) {
        if (!_chime.active()) {
            close();
        }
        return;
    }

    if (completed_now) {
        _completion_animation_start_us = now_us;
        _last_animation_frame_us = 0;
        _current_pose = focus_timer::Pose::Idle;
        if (_preferences.sound_enabled && !_chime.start() && !_audio_failure_logged) {
            ESP_LOGW(TAG, "completion chime could not start; visual completion remains available");
            _audio_failure_logged = true;
        }
    }

    const focus_timer::Action action = _pending_action.exchange(focus_timer::Action::None);
    if (action != focus_timer::Action::None) {
        handle_action(action, now_us);
    }

    update_animation(now_us);
    {
        LvglLockGuard lock;
        if (_view) {
            _view->update(make_view_state(now_us));
        }
    }
    save_preferences_if_changed();
}

void AppFocusTimer::onClose()
{
    _timer.cancel();
    _home_closing = true;
    _chime.request_cancel();
    _chime.shutdown();

    LvglLockGuard lock;
    _view.reset();
    view::destroy_home_indicator();
    view::destroy_status_bar();
    _pending_action.store(focus_timer::Action::None);
    _home_requested.store(false);
    _completion_animation_start_us = 0;
    _last_animation_frame_us = 0;
    _current_pose = focus_timer::Pose::Idle;
}

void AppFocusTimer::queue_action(focus_timer::Action action)
{
    _pending_action.store(action);
}

void AppFocusTimer::load_preferences()
{
    Settings settings(kSettingsNamespace, false);
    _preferences.preset = static_cast<uint8_t>(std::clamp<int32_t>(settings.GetInt("preset", 0), 0, 2));
    _preferences.focus_minutes =
        static_cast<uint8_t>(std::clamp<int32_t>(settings.GetInt("focus_minutes", 25), 1, 120));
    _preferences.break_minutes =
        static_cast<uint8_t>(std::clamp<int32_t>(settings.GetInt("break_minutes", 5), 1, 30));
    _preferences.sound_enabled = settings.GetBool("sound_enabled", true);
    _stored_preferences = _preferences;
}

void AppFocusTimer::save_preferences_if_changed()
{
    const bool preset_changed = _preferences.preset != _stored_preferences.preset;
    const bool focus_changed = _preferences.focus_minutes != _stored_preferences.focus_minutes;
    const bool break_changed = _preferences.break_minutes != _stored_preferences.break_minutes;
    const bool sound_changed = _preferences.sound_enabled != _stored_preferences.sound_enabled;
    if (!preset_changed && !focus_changed && !break_changed && !sound_changed) {
        return;
    }

    Settings settings(kSettingsNamespace, true);
    if (preset_changed) {
        settings.SetInt("preset", _preferences.preset);
        _stored_preferences.preset = _preferences.preset;
    }
    if (focus_changed) {
        settings.SetInt("focus_minutes", _preferences.focus_minutes);
        _stored_preferences.focus_minutes = _preferences.focus_minutes;
    }
    if (break_changed) {
        settings.SetInt("break_minutes", _preferences.break_minutes);
        _stored_preferences.break_minutes = _preferences.break_minutes;
    }
    if (sound_changed) {
        settings.SetBool("sound_enabled", _preferences.sound_enabled);
        _stored_preferences.sound_enabled = _preferences.sound_enabled;
    }
}

void AppFocusTimer::handle_action(focus_timer::Action action, uint64_t now_us)
{
    using focus_timer::Action;
    using focus_timer::Phase;
    using focus_timer::State;

    switch (action) {
        case Action::Select25:
            _preferences.preset = 0;
            break;
        case Action::Select50:
            _preferences.preset = 1;
            break;
        case Action::SelectCustom:
            _preferences.preset = 2;
            break;
        case Action::FocusMinus:
            _preferences.preset = 2;
            if (_preferences.focus_minutes > 1) {
                --_preferences.focus_minutes;
            }
            break;
        case Action::FocusPlus:
            _preferences.preset = 2;
            if (_preferences.focus_minutes < 120) {
                ++_preferences.focus_minutes;
            }
            break;
        case Action::BreakMinus:
            _preferences.preset = 2;
            if (_preferences.break_minutes > 1) {
                --_preferences.break_minutes;
            }
            break;
        case Action::BreakPlus:
            _preferences.preset = 2;
            if (_preferences.break_minutes < 30) {
                ++_preferences.break_minutes;
            }
            break;
        case Action::ToggleSound:
            _preferences.sound_enabled = !_preferences.sound_enabled;
            if (!_preferences.sound_enabled) {
                _chime.request_cancel();
            }
            break;
        case Action::Start:
            if (_timer.state() == State::Ready) {
                const uint8_t focus_minutes = _preferences.preset == 0 ? 25 :
                                              _preferences.preset == 1 ? 50 : _preferences.focus_minutes;
                _timer.start(Phase::Focus, static_cast<uint32_t>(focus_minutes) * 60U, now_us);
            }
            break;
        case Action::PauseResume:
            if (_timer.state() == State::Running) {
                _timer.pause(now_us);
            } else if (_timer.state() == State::Paused) {
                _timer.resume(now_us);
            }
            break;
        case Action::Extend:
            if (_timer.extend(now_us)) {
                _completion_animation_start_us = 0;
                _last_animation_frame_us = 0;
                if (_timer.state() == State::Running) {
                    _chime.request_cancel();
                }
            }
            break;
        case Action::Finish:
            _timer.cancel();
            _completion_animation_start_us = 0;
            _last_animation_frame_us = 0;
            _current_pose = focus_timer::Pose::Idle;
            _chime.request_cancel();
            break;
        case Action::StartBreak:
            if (_timer.state() == State::Completed && _timer.phase() == Phase::Focus) {
                const uint8_t break_minutes = _preferences.preset == 0 ? 5 :
                                              _preferences.preset == 1 ? 10 : _preferences.break_minutes;
                _timer.start(Phase::Break, static_cast<uint32_t>(break_minutes) * 60U, now_us);
                _completion_animation_start_us = 0;
                _last_animation_frame_us = 0;
                _chime.request_cancel();
            }
            break;
        case Action::StartFocus:
            if (_timer.state() == State::Completed && _timer.phase() == Phase::Break) {
                const uint8_t focus_minutes = _preferences.preset == 0 ? 25 :
                                              _preferences.preset == 1 ? 50 : _preferences.focus_minutes;
                _timer.start(Phase::Focus, static_cast<uint32_t>(focus_minutes) * 60U, now_us);
                _completion_animation_start_us = 0;
                _last_animation_frame_us = 0;
                _chime.request_cancel();
            }
            break;
        case Action::None:
            break;
    }
}

void AppFocusTimer::begin_home_close()
{
    _pending_action.store(focus_timer::Action::None);
    _timer.cancel();
    _home_closing = true;
    _completion_animation_start_us = 0;
    _last_animation_frame_us = 0;
    _current_pose = focus_timer::Pose::Idle;
    _chime.request_cancel();
    {
        LvglLockGuard lock;
        if (_view) {
            _view->show_closing();
        }
    }
}

void AppFocusTimer::update_animation(uint64_t now_us)
{
    using focus_timer::Phase;
    using focus_timer::Pose;
    using focus_timer::State;

    const State state = _timer.state();
    if (state == State::Ready) {
        _current_pose = Pose::Idle;
        _last_animation_frame_us = now_us;
        return;
    }
    if (state == State::Paused) {
        return;  // Preserve the pose until the timer resumes.
    }
    if (_last_animation_frame_us != 0 && now_us - _last_animation_frame_us < kAnimationFrameUs) {
        return;
    }

    if (state == State::Running && _timer.phase() == Phase::Break) {
        const uint64_t cycle_position = _timer.elapsed_us(now_us) % kWalkAndStretchCycleUs;
        if (cycle_position < 1500000ULL) {
            _current_pose = ((cycle_position / 300000ULL) % 2 == 0) ? Pose::WalkA : Pose::WalkB;
        } else if (cycle_position < 1900000ULL) {
            _current_pose = Pose::StretchStart;
        } else {
            _current_pose = Pose::Stretch;
        }
    } else if (state == State::Completed) {
        const uint64_t elapsed = _completion_animation_start_us == 0 || now_us < _completion_animation_start_us
                                     ? 0
                                     : now_us - _completion_animation_start_us;
        if (_timer.phase() == Phase::Focus) {
            if (elapsed < 600000ULL) {
                _current_pose = Pose::StretchStart;
            } else if (elapsed < 1900000ULL) {
                _current_pose = Pose::Stretch;
            } else if (elapsed < 2500000ULL) {
                _current_pose = Pose::StretchRelease;
            } else {
                _current_pose = Pose::Stretch;
            }
        } else if (elapsed >= kCompletionAnimationUs) {
            _current_pose = Pose::WaveRight;
        } else {
            _current_pose = ((elapsed / 250000ULL) % 2 == 0) ? Pose::WaveLeft : Pose::WaveRight;
        }
    } else {
        _current_pose = Pose::Idle;
    }
    _last_animation_frame_us = now_us;
}

focus_timer::ViewState AppFocusTimer::make_view_state(uint64_t now_us) const
{
    focus_timer::ViewState state;
    state.state = _timer.state();
    state.phase = _timer.phase();
    state.pose = _current_pose;
    state.remaining_seconds = _timer.remaining_seconds(now_us);
    state.progress_per_mille = _timer.progress_per_mille(now_us);
    state.preset = _preferences.preset;
    state.focus_minutes = _preferences.focus_minutes;
    state.break_minutes = _preferences.break_minutes;
    state.sound_enabled = _preferences.sound_enabled;
    return state;
}
