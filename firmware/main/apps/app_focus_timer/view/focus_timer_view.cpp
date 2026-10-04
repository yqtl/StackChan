/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "focus_timer_view.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>

LV_FONT_DECLARE(font_puhui_basic_20_4);
LV_FONT_DECLARE(font_puhui_basic_14_1);

namespace focus_timer {
namespace {

constexpr uint32_t kBackground = 0x083B4C;
constexpr uint32_t kButton = 0x104F61;
constexpr uint32_t kPrimary = 0xFFD166;
constexpr uint32_t kText = 0xD9F3F0;
constexpr uint32_t kMuted = 0x9CC7C9;
constexpr uint32_t kCharacter = 0xFFD166;
constexpr uint32_t kBody = 0x57C4C4;
constexpr int kCharacterX = 126;
constexpr int kCharacterY = 100;

void set_visible(lv_obj_t* object, bool visible)
{
    if (visible) {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

}  // namespace

FocusTimerView::FocusTimerView(std::function<void(Action)> on_action) : _on_action(std::move(on_action))
{
    _root = std::make_unique<Container>(lv_screen_active());
    _root->setSize(320, 240);
    _root->setAlign(LV_ALIGN_CENTER);
    _root->setRadius(0);
    _root->setBorderWidth(0);
    _root->setPadding(0, 0, 0, 0);
    _root->setScrollbarMode(LV_SCROLLBAR_MODE_OFF);
    _root->setBgColor(lv_color_hex(kBackground));
    _root->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _ready_title = make_label("FOCUS TIMER", 10, 26, 300, 22, &font_puhui_basic_20_4, kText);
    _preset_buttons[0] = make_button("25 / 5", 8, 50, 96, 44, Action::Select25);
    _preset_buttons[1] = make_button("50 / 10", 112, 50, 96, 44, Action::Select50);
    _preset_buttons[2] = make_button("Custom", 216, 50, 96, 44, Action::SelectCustom);

    _focus_minus = make_button("-", 10, 98, 44, 44, Action::FocusMinus);
    _focus_value = make_button("F 25m", 56, 98, 60, 44, Action::None, kBackground);
    _focus_plus = make_button("+", 118, 98, 44, 44, Action::FocusPlus);
    _break_minus = make_button("-", 164, 98, 44, 44, Action::BreakMinus);
    _break_value = make_button("B 5m", 210, 98, 60, 44, Action::None, kBackground);
    _break_plus = make_button("+", 272, 98, 44, 44, Action::BreakPlus);
    _sound_button = make_button("Sound On", 8, 145, 82, 44, Action::ToggleSound);
    _start_button = make_button("Start", 98, 145, 214, 44, Action::Start, kPrimary);

    _run_title = make_label("FOCUS", 10, 26, 300, 22, &font_puhui_basic_20_4, kText);
    _countdown = make_label("00:00", 10, 48, 300, 42, &lv_font_montserrat_24, kPrimary);
    _progress = lv_bar_create(_root->get());
    lv_obj_set_size(_progress, 280, 8);
    lv_obj_set_pos(_progress, 20, 92);
    lv_bar_set_range(_progress, 0, 1000);
    lv_bar_set_value(_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(_progress, lv_color_hex(kButton), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_progress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(_progress, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(_progress, lv_color_hex(kPrimary), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(_progress, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(_progress, 4, LV_PART_INDICATOR);

    create_character();
    _run_first = make_button("Pause", 9, 145, 94, 44, Action::None);
    _run_first->onClick().connect([this]() { submit(_run_first_action); });
    _run_extend = make_button("+5 min", 113, 145, 94, 44, Action::Extend);
    _run_finish = make_button("Finish", 217, 145, 94, 44, Action::Finish);

    set_ready_hidden(false);
    set_visible(_run_title->get(), false);
    set_visible(_countdown->get(), false);
    set_visible(_progress, false);
    for (auto* limb : _limbs) {
        set_visible(limb, false);
    }
    set_visible(_head, false);
    set_visible(_body, false);
    set_visible(_eye_left, false);
    set_visible(_eye_right, false);
    set_visible(_mouth, false);
    set_visible(_run_first->get(), false);
    set_visible(_run_extend->get(), false);
    set_visible(_run_finish->get(), false);
}

FocusTimerView::~FocusTimerView() = default;

std::unique_ptr<FocusTimerView::Button> FocusTimerView::make_button(const char* text, int x, int y,
                                                                    int width, int height, Action action,
                                                                    uint32_t color)
{
    auto button = std::make_unique<Button>(_root->get());
    button->setSize(width, height);
    button->setPos(x, y);
    button->setBgColor(lv_color_hex(color));
    button->setBgOpa(LV_OPA_COVER);
    button->setBorderWidth(color == kBackground ? 1 : 0);
    button->setBorderColor(lv_color_hex(kButton));
    button->setRadius(10);
    button->setShadowWidth(0);
    button->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    button->label().setText(text);
    button->label().setTextFont(&font_puhui_basic_14_1);
    button->label().setTextColor(lv_color_hex(color == kPrimary ? kBackground : kText));
    if (action != Action::None) {
        button->onClick().connect([this, action]() { submit(action); });
    }
    return button;
}

std::unique_ptr<FocusTimerView::Label> FocusTimerView::make_label(const char* text, int x, int y,
                                                                  int width, int height, const lv_font_t* font,
                                                                  uint32_t color, bool centered)
{
    auto label = std::make_unique<Label>(_root->get());
    label->setText(text);
    label->setTextFont(font);
    label->setTextColor(lv_color_hex(color));
    label->setTextAlign(centered ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT);
    label->setWidth(width);
    label->setHeight(height);
    label->setLongMode(LV_LABEL_LONG_MODE_CLIP);
    label->setPos(x, y);
    return label;
}

void FocusTimerView::create_character()
{
    auto create_shape = [this](int x, int y, int width, int height, uint32_t color, int radius) {
        lv_obj_t* shape = lv_obj_create(_root->get());
        lv_obj_set_pos(shape, kCharacterX + x, kCharacterY + y);
        lv_obj_set_size(shape, width, height);
        lv_obj_set_style_bg_color(shape, lv_color_hex(color), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(shape, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(shape, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(shape, radius, LV_PART_MAIN);
        lv_obj_remove_flag(shape, LV_OBJ_FLAG_SCROLLABLE);
        return shape;
    };

    _head = create_shape(20, 2, 28, 23, kCharacter, 8);
    _body = create_shape(25, 27, 18, 12, kBody, 5);
    _eye_left = create_shape(27, 10, 3, 4, kBackground, 2);
    _eye_right = create_shape(38, 10, 3, 4, kBackground, 2);
    _mouth = create_shape(31, 18, 7, 2, kBackground, 1);

    for (auto& limb : _limbs) {
        limb = lv_line_create(_root->get());
        lv_obj_set_pos(limb, kCharacterX, kCharacterY);
        lv_obj_set_size(limb, 68, 46);
        lv_obj_set_style_line_color(limb, lv_color_hex(kText), LV_PART_MAIN);
        lv_obj_set_style_line_width(limb, 3, LV_PART_MAIN);
        lv_obj_set_style_line_rounded(limb, true, LV_PART_MAIN);
        lv_obj_remove_flag(limb, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(limb, LV_OBJ_FLAG_SCROLLABLE);
    }
    set_pose(Pose::Idle);
}

void FocusTimerView::set_pose(Pose pose)
{
    if (_have_pose && pose == _last_pose) {
        return;
    }
    _last_pose = pose;
    _have_pose = true;

    using Point = lv_point_precise_t;
    Point left_arm_start{22, 27};
    Point right_arm_start{46, 27};
    Point left_arm_end{10, 38};
    Point right_arm_end{58, 38};
    Point left_leg_start{29, 36};
    Point right_leg_start{41, 36};
    Point left_leg_end{24, 44};
    Point right_leg_end{47, 44};

    switch (pose) {
        case Pose::Idle:
            break;
        case Pose::WalkA:
            left_arm_end = {12, 31};
            right_arm_end = {57, 43};
            left_leg_end = {19, 44};
            right_leg_end = {51, 39};
            break;
        case Pose::WalkB:
            left_arm_end = {11, 43};
            right_arm_end = {58, 31};
            left_leg_end = {25, 39};
            right_leg_end = {56, 44};
            break;
        case Pose::StretchStart:
            left_arm_end = {15, 11};
            right_arm_end = {53, 11};
            break;
        case Pose::Stretch:
            left_arm_end = {12, 3};
            right_arm_end = {56, 3};
            break;
        case Pose::StretchRelease:
            left_arm_end = {8, 14};
            right_arm_end = {60, 14};
            break;
        case Pose::WaveLeft:
            left_arm_end = {9, 8};
            right_arm_end = {59, 37};
            break;
        case Pose::WaveRight:
            left_arm_end = {9, 8};
            right_arm_end = {60, 5};
            break;
    }

    const std::array<std::array<Point, 2>, 4> points = {{{left_arm_start, left_arm_end},
                                                         {right_arm_start, right_arm_end},
                                                         {left_leg_start, left_leg_end},
                                                         {right_leg_start, right_leg_end}}};
    for (std::size_t limb = 0; limb < _limbs.size(); ++limb) {
        _limb_points[limb] = points[limb];
        lv_line_set_points(_limbs[limb], _limb_points[limb].data(), 2);
    }
}

void FocusTimerView::submit(Action action)
{
    if (_on_action && action != Action::None) {
        _on_action(action);
    }
}

void FocusTimerView::set_ready_hidden(bool hidden)
{
    set_visible(_ready_title->get(), !hidden);
    for (auto& button : _preset_buttons) {
        set_visible(button->get(), !hidden);
    }
    set_visible(_focus_minus->get(), !hidden);
    set_visible(_focus_value->get(), !hidden);
    set_visible(_focus_plus->get(), !hidden);
    set_visible(_break_minus->get(), !hidden);
    set_visible(_break_value->get(), !hidden);
    set_visible(_break_plus->get(), !hidden);
    set_visible(_sound_button->get(), !hidden);
    set_visible(_start_button->get(), !hidden);
}

void FocusTimerView::set_button_selected(Button& button, bool selected)
{
    button.setBgColor(lv_color_hex(selected ? kPrimary : kButton));
    button.label().setTextColor(lv_color_hex(selected ? kBackground : kText));
}

void FocusTimerView::update(const ViewState& state)
{
    const bool ready = state.state == State::Ready;
    if (_showing_ready != ready) {
        _showing_ready = ready;
        set_ready_hidden(!ready);
        const bool run_visible = !ready;
        set_visible(_run_title->get(), run_visible);
        set_visible(_countdown->get(), run_visible);
        set_visible(_progress, run_visible);
        for (auto* limb : _limbs) {
            set_visible(limb, run_visible);
        }
        set_visible(_head, run_visible);
        set_visible(_body, run_visible);
        set_visible(_eye_left, run_visible);
        set_visible(_eye_right, run_visible);
        set_visible(_mouth, run_visible);
        set_visible(_run_first->get(), run_visible);
        set_visible(_run_extend->get(), run_visible);
        set_visible(_run_finish->get(), run_visible);
        _have_countdown = false;
    }

    if (ready) {
        if (_last_preset != state.preset) {
            set_button_selected(*_preset_buttons[0], state.preset == 0);
            set_button_selected(*_preset_buttons[1], state.preset == 1);
            set_button_selected(*_preset_buttons[2], state.preset == 2);
            _last_preset = state.preset;
        }
        if (_last_focus_minutes != state.focus_minutes) {
            char text[12];
            std::snprintf(text, sizeof(text), "F %um", state.focus_minutes);
            _focus_value->label().setText(text);
            _last_focus_minutes = state.focus_minutes;
        }
        if (_last_break_minutes != state.break_minutes) {
            char text[12];
            std::snprintf(text, sizeof(text), "B %um", state.break_minutes);
            _break_value->label().setText(text);
            _last_break_minutes = state.break_minutes;
        }
        if (_last_sound_enabled != state.sound_enabled || !_have_countdown) {
            _sound_button->label().setText(state.sound_enabled ? "Sound On" : "Sound Off");
            _last_sound_enabled = state.sound_enabled;
        }
        _have_countdown = true;
        return;
    }

    if (!_have_run_header || _last_run_state != state.state || _last_run_phase != state.phase) {
        if (state.state == State::Completed) {
            _run_title->setText(state.phase == Phase::Focus ? "Focus complete" : "Break complete");
            _run_first_action = state.phase == Phase::Focus ? Action::StartBreak : Action::StartFocus;
            _run_first->label().setText(state.phase == Phase::Focus ? "Start break" : "Start focus");
        } else if (state.state == State::Paused) {
            _run_title->setText(state.phase == Phase::Focus ? "Focus paused" : "Break paused");
            _run_first_action = Action::PauseResume;
            _run_first->label().setText("Resume");
        } else {
            _run_title->setText(state.phase == Phase::Focus ? "Focus" : "Break");
            _run_first_action = Action::PauseResume;
            _run_first->label().setText("Pause");
        }
        _last_run_state = state.state;
        _last_run_phase = state.phase;
        _have_run_header = true;
    }

    if (!_have_countdown || _last_countdown_seconds != state.remaining_seconds) {
        const uint32_t minutes = state.remaining_seconds / 60;
        const uint32_t seconds = state.remaining_seconds % 60;
        char text[16];
        std::snprintf(text, sizeof(text), "%02lu:%02lu",
                      static_cast<unsigned long>(minutes), static_cast<unsigned long>(seconds));
        _countdown->setText(text);
        _last_countdown_seconds = state.remaining_seconds;
        _have_countdown = true;
    }
    if (_last_progress != state.progress_per_mille) {
        lv_bar_set_value(_progress, state.progress_per_mille, LV_ANIM_OFF);
        _last_progress = state.progress_per_mille;
    }
    set_pose(state.pose);
}

void FocusTimerView::show_closing()
{
    _run_title->setText("Closing");
    _ready_title->setText("Closing");
}

}  // namespace focus_timer
