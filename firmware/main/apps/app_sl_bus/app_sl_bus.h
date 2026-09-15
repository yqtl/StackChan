/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "sl_bus_departure_client.h"
#include "sl_bus_departures.h"
#include "view/view.h"

#include <atomic>
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
    bool wait_interruptibly(uint32_t seconds);
    void stop_worker();

    sl_bus::DepartureStore _store;
    std::unique_ptr<view::SlBusView> _view;
    std::atomic<bool> _stop{true};
    std::atomic<bool> _network_waiting{false};
    SemaphoreHandle_t _done = nullptr;
    SemaphoreHandle_t _wake = nullptr;
    bool _worker_started = false;
};
