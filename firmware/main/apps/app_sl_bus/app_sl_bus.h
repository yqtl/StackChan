/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "sl_bus_departure_client.h"
#include "sl_bus_close_state.hpp"
#include "sl_bus_departures.h"
#include "sl_bus_poll_schedule.h"
#include "view/view.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mooncake.h>

class AppSlBus : public mooncake::AppAbility {
public:
    AppSlBus();
    ~AppSlBus() override;

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    static void worker(void* context);
    void run_worker();
    bool wait_interruptibly_ms(uint64_t duration_ms);
    void request_home();
    bool poll_worker_completion();
    void publish_cooldown_state(std::size_t skip_stop = sl_bus::kStopCount);
    void stop_worker();

    sl_bus::DepartureStore _store;
    sl_bus::PollSchedule _schedule;
    sl_bus::SlBusCloseState _close_state;
    sl_bus::DepartureSnapshot _cached_snapshot;
    std::unique_ptr<view::SlBusView> _view;
    std::atomic<bool> _stop{true};
    std::atomic<bool> _network_waiting{false};
    SemaphoreHandle_t _done = nullptr;
    SemaphoreHandle_t _wake = nullptr;
    bool _worker_started = false;
    bool _busy_published = false;
    int64_t _drain_started_us = 0;
    bool _drain_warning_logged = false;
};
