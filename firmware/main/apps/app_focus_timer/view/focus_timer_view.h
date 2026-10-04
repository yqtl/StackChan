/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>

#include <lvgl.h>
#include <smooth_lvgl.hpp>
#include <uitk/short_namespace.hpp>

#include "../focus_timer_model.hpp"

namespace focus_timer {

enum class Action : uint8_t {
    None,
    Select25,
    Select50,
    SelectCustom,
    FocusMinus,
    FocusPlus,
    BreakMinus,
    BreakPlus,
    ToggleSound,
    Start,
    PauseResume,
    Extend,
    Finish,
    StartBreak,
    StartFocus,
};

enum class Pose : uint8_t {
    Idle,
    WalkA,
    WalkB,
    StretchStart,
    Stretch,
    StretchRelease,
    WaveLeft,
    WaveRight,
};

struct ViewState {
    State state = State::Ready;
    Phase phase = Phase::Focus;
    Pose pose = Pose::Idle;
    uint32_t remaining_seconds = 0;
    uint16_t progress_per_mille = 0;
    uint8_t preset = 0;
    uint8_t focus_minutes = 25;
    uint8_t break_minutes = 5;
    bool sound_enabled = true;
};

class FocusTimerView {
public:
    explicit FocusTimerView(std::function<void(Action)> on_action);
    ~FocusTimerView();

    FocusTimerView(const FocusTimerView&) = delete;
    FocusTimerView& operator=(const FocusTimerView&) = delete;

    void update(const ViewState& state);
    void show_closing();

private:
    using Button = uitk::lvgl_cpp::Button;
    using Label = uitk::lvgl_cpp::Label;
    using Container = uitk::lvgl_cpp::Container;

    std::unique_ptr<Container> _root;
    std::unique_ptr<Label> _ready_title;
    std::array<std::unique_ptr<Button>, 3> _preset_buttons;
    std::unique_ptr<Button> _focus_minus;
    std::unique_ptr<Button> _focus_value;
    std::unique_ptr<Button> _focus_plus;
    std::unique_ptr<Button> _break_minus;
    std::unique_ptr<Button> _break_value;
    std::unique_ptr<Button> _break_plus;
    std::unique_ptr<Button> _sound_button;
    std::unique_ptr<Button> _start_button;

    std::unique_ptr<Label> _run_title;
    std::unique_ptr<Label> _countdown;
    lv_obj_t* _progress = nullptr;
    std::unique_ptr<Button> _run_first;
    std::unique_ptr<Button> _run_extend;
    std::unique_ptr<Button> _run_finish;
    Action _run_first_action = Action::PauseResume;

    lv_obj_t* _head = nullptr;
    lv_obj_t* _body = nullptr;
    lv_obj_t* _eye_left = nullptr;
    lv_obj_t* _eye_right = nullptr;
    lv_obj_t* _mouth = nullptr;
    std::array<lv_obj_t*, 4> _limbs{};
    std::array<std::array<lv_point_precise_t, 2>, 4> _limb_points{};

    std::function<void(Action)> _on_action;
    bool _showing_ready = true;
    bool _have_countdown = false;
    uint32_t _last_countdown_seconds = 0;
    uint16_t _last_progress = UINT16_MAX;
    Pose _last_pose = Pose::Idle;
    bool _have_pose = false;
    uint8_t _last_preset = UINT8_MAX;
    uint8_t _last_focus_minutes = UINT8_MAX;
    uint8_t _last_break_minutes = UINT8_MAX;
    bool _last_sound_enabled = false;
    bool _have_run_header = false;
    State _last_run_state = State::Ready;
    Phase _last_run_phase = Phase::Focus;

    std::unique_ptr<Button> make_button(const char* text, int x, int y, int width, int height,
                                        Action action, uint32_t color = 0x104F61);
    std::unique_ptr<Label> make_label(const char* text, int x, int y, int width, int height,
                                      const lv_font_t* font, uint32_t color, bool centered = true);
    void create_character();
    void set_pose(Pose pose);
    void submit(Action action);
    void set_ready_hidden(bool hidden);
    void set_button_selected(Button& button, bool selected);
};

}  // namespace focus_timer
