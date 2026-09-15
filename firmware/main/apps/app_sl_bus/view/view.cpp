/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "view.h"

#include <string>

LV_FONT_DECLARE(font_puhui_basic_20_4);
LV_FONT_DECLARE(font_puhui_basic_14_1);

namespace view {
namespace {

using uitk::lvgl_cpp::Container;
using uitk::lvgl_cpp::Label;

constexpr uint32_t kBackground = 0x083B4C;
constexpr uint32_t kPrimary = 0xFFD166;
constexpr uint32_t kSecondary = 0xD9F3F0;
constexpr uint32_t kMuted = 0x9CC7C9;
constexpr uint32_t kWarning = 0xFF9F1C;

constexpr int kPanelWidth = 152;
constexpr int kPanelHeight = 158;
constexpr int kPanelTop = 30;
constexpr int kPanelGap = 8;
constexpr int kPanelMargin = 4;
constexpr int kContentPadding = 6;
constexpr int kContentWidth = kPanelWidth - (2 * kContentPadding);

void setup_text(Label& label, const lv_font_t* font, lv_color_t color, int width, int height)
{
    label.setTextFont(font);
    label.setTextColor(color);
    label.setWidth(width);
    label.setHeight(height);
    label.setTextAlign(LV_TEXT_ALIGN_LEFT);
    label.setLongMode(LV_LABEL_LONG_MODE_CLIP);
}

bool has_multiple_destinations(const sl_bus::StopSnapshot& snapshot)
{
    if (snapshot.departure_count < 2) {
        return false;
    }

    const auto& first_destination = snapshot.departures[0].destination;
    for (std::size_t i = 1; i < snapshot.departure_count; ++i) {
        if (snapshot.departures[i].destination != first_destination) {
            return true;
        }
    }
    return false;
}

std::string route_text(std::size_t stop_index, const sl_bus::StopSnapshot& snapshot)
{
    std::string text(sl_bus::kStops[stop_index].line);
    if (!has_multiple_destinations(snapshot)) {
        text += " ";
        if (snapshot.departure_count > 0 && !snapshot.departures[0].destination.empty()) {
            text += snapshot.departures[0].destination;
        } else {
            text += sl_bus::kStops[stop_index].direction;
        }
    }
    return text;
}

std::string destination_text(const sl_bus::Departure& departure)
{
    std::string text = departure.destination;
    if (departure.cancelled) {
        text += " · Cancelled";
    }
    return text;
}

std::string note_text(const sl_bus::StopSnapshot& snapshot)
{
    if (snapshot.fetch_status == sl_bus::FetchStatus::Failed && !snapshot.error.empty()) {
        return snapshot.error;
    }
    if (snapshot.notice_count > 0) {
        std::string text = snapshot.notices[0].message;
        if (!snapshot.notices[0].consequence.empty()) {
            text += " · " + snapshot.notices[0].consequence;
        }
        return text;
    }
    for (std::size_t i = 0; i < snapshot.departure_count; ++i) {
        if (!snapshot.departures[i].disruption.empty()) {
            return snapshot.departures[i].destination + ": " + snapshot.departures[i].disruption;
        }
    }
    return {};
}

std::string message_text(const sl_bus::StopSnapshot& snapshot, bool network_waiting)
{
    if (network_waiting && snapshot.fetch_status == sl_bus::FetchStatus::Fetching) {
        return "Connecting";
    }
    if (snapshot.fetch_status == sl_bus::FetchStatus::Fetching) {
        return "Updating";
    }
    if (snapshot.fetch_status == sl_bus::FetchStatus::Failed) {
        return snapshot.error.empty() ? "Refresh failed" : snapshot.error;
    }
    if (snapshot.expired) {
        return "Outdated";
    }
    if (snapshot.fetch_status == sl_bus::FetchStatus::Empty) {
        return "No departures";
    }
    return note_text(snapshot);
}

}  // namespace

SlBusView::SlBusView()
{
    _root = std::make_unique<Container>(lv_screen_active());
    _root->setSize(320, 240);
    _root->align(LV_ALIGN_CENTER, 0, 0);
    _root->setRadius(0);
    _root->setBorderWidth(0);
    _root->setPadding(0, 0, 0, 0);
    _root->setScrollbarMode(LV_SCROLLBAR_MODE_OFF);
    _root->setBgColor(lv_color_hex(kBackground));
    _root->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    _root->removeFlag(LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    for (std::size_t stop_index = 0; stop_index < sl_bus::kStopCount; ++stop_index) {
        auto& widgets = _stops[stop_index];
        widgets.panel = std::make_unique<Container>(_root->get());
        widgets.panel->setSize(kPanelWidth, kPanelHeight);
        widgets.panel->setPos(
            static_cast<int>(kPanelMargin + stop_index * (kPanelWidth + kPanelGap)), kPanelTop);
        widgets.panel->setRadius(8);
        widgets.panel->setBorderWidth(0);
        widgets.panel->setPadding(0, 0, 0, 0);
        widgets.panel->setScrollbarMode(LV_SCROLLBAR_MODE_OFF);
        widgets.panel->setBgColor(lv_color_hex(0x104F61));
        widgets.panel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
        widgets.panel->removeFlag(LV_OBJ_FLAG_OVERFLOW_VISIBLE);

        widgets.stop = std::make_unique<Label>(widgets.panel->get());
        setup_text(*widgets.stop, &font_puhui_basic_14_1, lv_color_hex(kSecondary), kContentWidth, 20);
        widgets.stop->setTextAlign(LV_TEXT_ALIGN_CENTER);
        widgets.stop->setPos(kContentPadding, 3);
        widgets.stop->setText(sl_bus::kStops[stop_index].name);

        widgets.route = std::make_unique<Label>(widgets.panel->get());
        setup_text(*widgets.route, &font_puhui_basic_20_4, lv_color_hex(kMuted), kContentWidth, 24);
        widgets.route->setTextAlign(LV_TEXT_ALIGN_CENTER);
        widgets.route->setPos(kContentPadding, 24);
        widgets.route->setText(route_text(stop_index, sl_bus::StopSnapshot{}));

        widgets.next_value = std::make_unique<Label>(widgets.panel->get());
        setup_text(*widgets.next_value, &lv_font_montserrat_24, lv_color_hex(kPrimary), kContentWidth, 32);
        widgets.next_value->setTextAlign(LV_TEXT_ALIGN_CENTER);
        widgets.next_value->setPos(kContentPadding, 49);

        widgets.next_destination = std::make_unique<Label>(widgets.panel->get());
        setup_text(*widgets.next_destination, &font_puhui_basic_14_1, lv_color_hex(kSecondary), kContentWidth, 18);
        widgets.next_destination->setTextAlign(LV_TEXT_ALIGN_CENTER);
        widgets.next_destination->setPos(kContentPadding, 81);

        for (std::size_t i = 0; i < widgets.later_values.size(); ++i) {
            const int y = static_cast<int>(85 + i * 24);
            widgets.later_values[i] = std::make_unique<Label>(widgets.panel->get());
            setup_text(*widgets.later_values[i], &lv_font_montserrat_16, lv_color_hex(kPrimary), kContentWidth, 22);
            widgets.later_values[i]->setTextAlign(LV_TEXT_ALIGN_CENTER);
            widgets.later_values[i]->setPos(kContentPadding, y);

            widgets.later_destinations[i] = std::make_unique<Label>(widgets.panel->get());
            setup_text(*widgets.later_destinations[i], &font_puhui_basic_14_1, lv_color_hex(kSecondary), 82, 18);
            widgets.later_destinations[i]->setPos(kContentPadding + 58, y);
        }

        widgets.message = std::make_unique<Label>(widgets.panel->get());
        setup_text(*widgets.message, &font_puhui_basic_14_1, lv_color_hex(kWarning), kContentWidth, 18);
        widgets.message->setTextAlign(LV_TEXT_ALIGN_CENTER);
        widgets.message->setPos(kContentPadding, 139);
    }

    sl_bus::DepartureSnapshot initial;
    render(initial, true);
}

SlBusView::~SlBusView() = default;

void SlBusView::update(const sl_bus::DepartureSnapshot& snapshot, bool network_waiting)
{
    std::array<bool, sl_bus::kStopCount> expired{};
    for (std::size_t stop_index = 0; stop_index < sl_bus::kStopCount; ++stop_index) {
        expired[stop_index] = snapshot.stops[stop_index].expired;
    }
    if (_last_generation == snapshot.generation && _last_expired == expired &&
        _last_network_waiting == network_waiting) {
        return;
    }
    render(snapshot, network_waiting);
    _last_generation = snapshot.generation;
    _last_expired = expired;
    _last_network_waiting = network_waiting;
}

void SlBusView::render(const sl_bus::DepartureSnapshot& snapshot, bool network_waiting)
{
    for (std::size_t stop_index = 0; stop_index < sl_bus::kStopCount; ++stop_index) {
        const auto& stop = snapshot.stops[stop_index];
        auto& widgets = _stops[stop_index];
        const bool show_times = !stop.expired && stop.departure_count > 0;
        const bool show_destinations = show_times && has_multiple_destinations(stop);
        const int later_y = show_destinations ? 99 : 85;
        const int later_step = show_destinations ? 20 : 24;
        const int message_y = show_destinations ? 140 : 139;

        widgets.stop->setText(sl_bus::kStops[stop_index].name);
        widgets.route->setText(route_text(stop_index, stop));
        widgets.next_destination->setHidden(!show_destinations);
        widgets.next_destination->setPos(kContentPadding, 81);
        for (std::size_t i = 0; i < widgets.later_values.size(); ++i) {
            widgets.later_destinations[i]->setHidden(!show_destinations);
            widgets.later_values[i]->setPos(kContentPadding, later_y + static_cast<int>(i * later_step));
            if (show_destinations) {
                widgets.later_values[i]->setHeight(20);
                widgets.later_values[i]->setWidth(54);
                widgets.later_values[i]->setTextAlign(LV_TEXT_ALIGN_RIGHT);
                widgets.later_destinations[i]->setWidth(82);
                widgets.later_destinations[i]->setPos(kContentPadding + 58,
                                                       later_y + static_cast<int>(i * later_step));
            } else {
                widgets.later_values[i]->setHeight(22);
                widgets.later_values[i]->setWidth(kContentWidth);
                widgets.later_values[i]->setTextAlign(LV_TEXT_ALIGN_CENTER);
            }
        }
        widgets.message->setPos(kContentPadding, message_y);

        if (show_times) {
            widgets.next_value->setText(stop.departures[0].display);
            widgets.next_destination->setText(show_destinations ? destination_text(stop.departures[0]) : "");
            for (std::size_t i = 0; i < widgets.later_values.size(); ++i) {
                const std::size_t departure_index = i + 1;
                if (departure_index < stop.departure_count) {
                    widgets.later_values[i]->setText(stop.departures[departure_index].display);
                    widgets.later_destinations[i]->setText(show_destinations
                                                               ? destination_text(stop.departures[departure_index])
                                                               : "");
                } else {
                    widgets.later_values[i]->setText("");
                    widgets.later_destinations[i]->setText("");
                }
            }
        } else {
            widgets.next_value->setText(stop.expired ? "--" : "None");
            widgets.next_destination->setText("");
            for (std::size_t i = 0; i < widgets.later_values.size(); ++i) {
                widgets.later_values[i]->setText("");
                widgets.later_destinations[i]->setText("");
            }
        }

        widgets.message->setText(message_text(stop, network_waiting));
    }
}

}  // namespace view
