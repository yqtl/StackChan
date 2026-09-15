/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "../sl_bus_departures.h"

#include <array>
#include <cstdint>
#include <memory>

#include <lvgl.h>
#include <smooth_lvgl.hpp>
#include <uitk/short_namespace.hpp>

namespace view {

class SlBusView {
public:
    SlBusView();
    ~SlBusView();

    void update(const sl_bus::DepartureSnapshot& snapshot, bool network_waiting);

private:
    struct StopWidgets {
        std::unique_ptr<uitk::lvgl_cpp::Container> panel;
        std::unique_ptr<uitk::lvgl_cpp::Label> stop;
        std::unique_ptr<uitk::lvgl_cpp::Label> route;
        std::unique_ptr<uitk::lvgl_cpp::Label> next_value;
        std::unique_ptr<uitk::lvgl_cpp::Label> next_destination;
        std::array<std::unique_ptr<uitk::lvgl_cpp::Label>, 2> later_values;
        std::array<std::unique_ptr<uitk::lvgl_cpp::Label>, 2> later_destinations;
        std::unique_ptr<uitk::lvgl_cpp::Label> message;
    };

    std::unique_ptr<uitk::lvgl_cpp::Container> _root;
    std::array<StopWidgets, sl_bus::kStopCount> _stops;

    uint32_t _last_generation = UINT32_MAX;
    std::array<bool, sl_bus::kStopCount> _last_expired{};
    bool _last_network_waiting = false;

    void render(const sl_bus::DepartureSnapshot& snapshot, bool network_waiting);
};

}  // namespace view
