/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "focus_chime.h"
#include "focus_timer_model.hpp"
#include "view/focus_timer_view.h"

#include <atomic>
#include <cstdint>
#include <memory>

#include <mooncake.h>

class AppFocusTimer : public mooncake::AppAbility {
public:
    AppFocusTimer();
    ~AppFocusTimer() override;

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    struct Preferences {
        uint8_t preset = 0;
        uint8_t focus_minutes = 25;
        uint8_t break_minutes = 5;
        bool sound_enabled = true;
    };

    void queue_action(focus_timer::Action action);
    void load_preferences();
    void save_preferences_if_changed();
    void handle_action(focus_timer::Action action, uint64_t now_us);
    void begin_home_close();
    void update_animation(uint64_t now_us);
    focus_timer::ViewState make_view_state(uint64_t now_us) const;

    focus_timer::TimerModel _timer;
    Preferences _preferences;
    Preferences _stored_preferences;
    std::unique_ptr<focus_timer::FocusTimerView> _view;
    FocusChime _chime;
    std::atomic<focus_timer::Action> _pending_action{focus_timer::Action::None};
    std::atomic<bool> _home_requested{false};
    uint64_t _completion_animation_start_us = 0;
    uint64_t _last_animation_frame_us = 0;
    focus_timer::Pose _current_pose = focus_timer::Pose::Idle;
    bool _home_closing = false;
    bool _audio_failure_logged = false;
};
