/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_sl_bus.h"

#include <apps/common/home_indicator/home_indicator.h>
#include <apps/common/status_bar/status_bar.h>
#include <assets/assets.h>
#include <esp_timer.h>
#include <freertos/task.h>
#include <hal/hal.h>
#include <mooncake_log.h>

#include <algorithm>
#include <cstdint>

namespace {

uint64_t monotonic_ms()
{
    return static_cast<uint64_t>(esp_timer_get_time() / 1000);
}

}  // namespace

AppSlBus::AppSlBus()
{
    setAppInfo().name = "SL.BUS";
    static auto icon = assets::get_image("icon_indicator_right.bin");
    setAppInfo().icon = (void*)&icon;
    static uint32_t theme_color = 0xFFD166;
    setAppInfo().userData = (void*)&theme_color;
}

AppSlBus::~AppSlBus()
{
    stop_worker();
}

void AppSlBus::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppSlBus::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");
    _store.reset();
    _stop.store(false);
    _network_waiting.store(true);

    {
        LvglLockGuard lock;
        _view = std::make_unique<view::SlBusView>();
        view::create_home_indicator([this]() { close(); }, 0xFFD166, 0x083B4C);
        view::create_status_bar(0xFFD166, 0x083B4C);
    }

    _done = xSemaphoreCreateBinary();
    _wake = xSemaphoreCreateBinary();
    if (!_done || !_wake) {
        _network_waiting.store(false);
        stop_worker();
        for (std::size_t stop_index = 0; stop_index < sl_bus::kStopCount; ++stop_index) {
            _store.publish_failure(stop_index, "Not enough memory for bus updater");
        }
        return;
    }

    _worker_started = xTaskCreatePinnedToCore(worker, "sl_bus", 8192, this, 2, nullptr, 0) == pdPASS;
    if (!_worker_started) {
        _network_waiting.store(false);
        stop_worker();
        for (std::size_t stop_index = 0; stop_index < sl_bus::kStopCount; ++stop_index) {
            _store.publish_failure(stop_index, "Cannot start bus updater");
        }
    }
}

void AppSlBus::onRunning()
{
    const auto snapshot = _store.snapshot(monotonic_ms());
    LvglLockGuard lock;
    if (_view) {
        _view->update(snapshot, _network_waiting.load());
    }
    view::update_home_indicator();
    view::update_status_bar();
}

void AppSlBus::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");
    stop_worker();

    LvglLockGuard lock;
    _view.reset();
    view::destroy_home_indicator();
    view::destroy_status_bar();
}

void AppSlBus::worker(void* context)
{
    auto* app = static_cast<AppSlBus*>(context);
    try {
        app->run_worker();
    } catch (...) {
        if (!app->_stop.load()) {
            app->_network_waiting.store(false);
            for (std::size_t stop_index = 0; stop_index < sl_bus::kStopCount; ++stop_index) {
                app->_store.publish_failure(stop_index, "Bus updater stopped unexpectedly");
            }
        }
    }

    // Do not touch app members after signalling. onClose() can destroy the app
    // immediately after taking this semaphore.
    const auto done = app->_done;
    xSemaphoreGive(done);
    vTaskDelete(nullptr);
}

void AppSlBus::run_worker()
{
    sl_bus::DepartureClient client([this]() { return _stop.load(); });
    uint32_t consecutive_failures = 0;

    while (!_stop.load()) {
        for (std::size_t stop_index = 0; stop_index < sl_bus::kStopCount; ++stop_index) {
            _store.set_fetching(stop_index);
        }
        _network_waiting.store(true);
        const bool connected = GetHAL().startNetworkCancellable(
            [this]() { return _stop.load(); }, [](std::string_view) {});
        _network_waiting.store(false);
        if (_stop.load()) {
            return;
        }

        if (!connected) {
            for (std::size_t stop_index = 0; stop_index < sl_bus::kStopCount; ++stop_index) {
                _store.publish_failure(stop_index, "Wi-Fi unavailable");
            }
            ++consecutive_failures;
            if (wait_interruptibly(sl_bus::retry_delay_seconds(consecutive_failures))) {
                return;
            }
            continue;
        }

        bool all_success = true;
        uint32_t server_retry_after = 0;
        for (std::size_t stop_index = 0; stop_index < sl_bus::kStopCount; ++stop_index) {
            const auto result = client.fetch(sl_bus::kStops[stop_index]);
            if (_stop.load() || result.status == sl_bus::FetchResultStatus::Cancelled) {
                return;
            }

            if (result.status == sl_bus::FetchResultStatus::Success) {
                _store.publish_success(stop_index, result.data, monotonic_ms());
                continue;
            }

            all_success = false;
            server_retry_after = std::max(server_retry_after, result.retry_after_seconds);
            _store.publish_failure(stop_index, result.error.empty() ? "SL update failed" : result.error);
        }

        if (all_success) {
            consecutive_failures = 0;
            if (wait_interruptibly(sl_bus::kPollIntervalSeconds)) {
                return;
            }
            continue;
        }

        ++consecutive_failures;
        const auto delay = sl_bus::retry_delay_seconds(consecutive_failures, server_retry_after);
        if (wait_interruptibly(delay)) {
            return;
        }
    }
}

bool AppSlBus::wait_interruptibly(uint32_t seconds)
{
    uint64_t remaining_ms = static_cast<uint64_t>(seconds) * 1000;
    while (!_stop.load() && remaining_ms > 0) {
        const uint32_t slice_ms = static_cast<uint32_t>(std::min<uint64_t>(remaining_ms, 1000));
        if (_wake && xSemaphoreTake(_wake, pdMS_TO_TICKS(slice_ms)) == pdTRUE) {
            return true;
        }
        remaining_ms -= slice_ms;
        GetHAL().feedTheDog();
    }
    return _stop.load();
}

void AppSlBus::stop_worker()
{
    _stop.store(true);
    _network_waiting.store(false);
    if (_wake) {
        xSemaphoreGive(_wake);
    }
    if (_worker_started) {
        while (xSemaphoreTake(_done, pdMS_TO_TICKS(50)) != pdTRUE) {
            GetHAL().feedTheDog();
        }
        _worker_started = false;
    }
    if (_wake) {
        vSemaphoreDelete(_wake);
        _wake = nullptr;
    }
    if (_done) {
        vSemaphoreDelete(_done);
        _done = nullptr;
    }
}
