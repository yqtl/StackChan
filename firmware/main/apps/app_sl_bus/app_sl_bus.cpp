/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_sl_bus.h"

#include <apps/common/home_indicator/home_indicator.h>
#include <apps/common/status_bar/status_bar.h>
#include <assets/assets.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/task.h>
#include <hal/hal.h>
#include <mooncake_log.h>

#include <algorithm>
#include <cstdint>
#include <limits>

namespace {

constexpr const char *TAG = "SL.BUS";
constexpr uint64_t kNetworkStartupTimeoutMs = 15 * 1000;
constexpr int64_t kDrainWarningUs = 5 * 1000 * 1000;

uint64_t monotonic_ms()
{
    return static_cast<uint64_t>(esp_timer_get_time() / 1000);
}

void log_heap_boundary(const char *event)
{
    ESP_LOGI(TAG, "%s t=%llu heap=%u largest=%u", event,
             static_cast<unsigned long long>(esp_timer_get_time() / 1000),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
}

const char* failure_kind_text(sl_bus::FetchFailureKind kind)
{
    switch (kind) {
        case sl_bus::FetchFailureKind::None:
            return "none";
        case sl_bus::FetchFailureKind::Transport:
            return "transport";
        case sl_bus::FetchFailureKind::HttpStatus:
            return "http-status";
        case sl_bus::FetchFailureKind::InvalidResponse:
            return "invalid-response";
        case sl_bus::FetchFailureKind::Resource:
            return "resource";
    }
    return "unknown";
}

uint64_t add_ms_saturated(uint64_t base_ms, uint64_t delay_ms)
{
    if (std::numeric_limits<uint64_t>::max() - base_ms < delay_ms) {
        return std::numeric_limits<uint64_t>::max();
    }
    return base_ms + delay_ms;
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
    // Reopening is allowed only after the previous worker has been joined.
    stop_worker();
    _store.reset();
    _schedule.begin_session(monotonic_ms());
    _cached_snapshot = {};
    _store.copy_if_changed(_cached_snapshot, true);
    _close_state.reset();
    _busy_published = false;
    _drain_started_us = 0;
    _drain_warning_logged = false;
    _stop.store(false);
    _network_waiting.store(true);

    {
        LvglLockGuard lock;
        _view = std::make_unique<view::SlBusView>();
        view::create_home_indicator([this]() { request_home(); }, 0xFFD166, 0x083B4C);
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
    log_heap_boundary("SL.BUS open/network start");
}

void AppSlBus::onRunning()
{
    {
        LvglLockGuard lock;
        // Home can invoke request_home synchronously from this update.  It
        // must win over a same-tick departure render.
        view::update_home_indicator();
        if (_close_state.is_draining()) {
            if (_view) {
                _view->show_closing();
            }
        } else {
            const auto now_ms = monotonic_ms();
            _store.copy_if_changed(_cached_snapshot);
            _store.update_expiry(_cached_snapshot, now_ms);
            if (_view) {
                _view->update(_cached_snapshot, _network_waiting.load());
            }
        }
        view::update_status_bar();
    }

    if (poll_worker_completion()) {
        if (_close_state.worker_completed() == sl_bus::SlBusCloseState::Action::Close) {
            close();
        }
    }

    if (_close_state.is_draining() && _drain_started_us > 0 && !_drain_warning_logged &&
        esp_timer_get_time() - _drain_started_us > kDrainWarningUs) {
        ESP_LOGW(TAG, "SL.BUS drain exceeded 5 seconds; waiting for worker completion");
        _drain_warning_logged = true;
    }
}

void AppSlBus::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");
    stop_worker();

    LvglLockGuard lock;
    _view.reset();
    view::destroy_home_indicator();
    view::destroy_status_bar();
    _close_state.reset();
    _busy_published = false;
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
    sl_bus::DepartureClient client([this]() { return _stop.load(); }, sl_bus::make_board_http_factory());

    while (!_stop.load()) {
        const auto now_ms = monotonic_ms();
        const bool cooldown_active = _schedule.cooldown_active(now_ms);
        if (!cooldown_active) {
            _busy_published = false;
        }

        if (cooldown_active) {
            publish_cooldown_state();
            const auto wake_ms = _schedule.next_wakeup_ms();
            if (wake_ms > now_ms && wait_interruptibly_ms(wake_ms - now_ms)) {
                return;
            }
            continue;
        }

        const auto due = _schedule.next_due(now_ms);
        if (!due) {
            const auto wake_ms = _schedule.next_wakeup_ms();
            if (wake_ms > now_ms && wait_interruptibly_ms(wake_ms - now_ms)) {
                return;
            }
            continue;
        }

        const std::size_t stop_index = *due;
        if (_stop.load()) {
            return;
        }
        _store.set_fetching(stop_index);

        _network_waiting.store(true);
        const auto network_deadline_ms = add_ms_saturated(monotonic_ms(), kNetworkStartupTimeoutMs);
        const bool connected = GetHAL().startNetworkCancellable(
            [this, network_deadline_ms]() {
                return _stop.load() || monotonic_ms() >= network_deadline_ms;
            },
            [](std::string_view) {});
        _network_waiting.store(false);
        if (_stop.load()) {
            return;
        }

        if (!connected) {
            const auto completed_ms = monotonic_ms();
            const sl_bus::FetchResult network_failure = {
                sl_bus::FetchResultStatus::Failed,
                {},
                "Wi-Fi unavailable",
                0,
                sl_bus::FetchFailureKind::Transport,
                0,
            };
            while (!_stop.load()) {
                const auto failed_stop = _schedule.next_due(completed_ms);
                if (!failed_stop) {
                    break;
                }
                _store.publish_failure(*failed_stop, "Wi-Fi unavailable");
                _schedule.record_failure(*failed_stop, network_failure, completed_ms);
            }
            if (_stop.load()) {
                return;
            }
            continue;
        }

        if (_stop.load()) {
            return;
        }

        const auto& stop = sl_bus::kStops[stop_index];
        auto result = client.fetch_with_retry(
            stop,
            [this, &stop](uint32_t delay_ms) {
                ESP_LOGW(TAG, "quick retry stop=%.*s delay_ms=%u",
                         static_cast<int>(stop.name.size()), stop.name.data(),
                         static_cast<unsigned>(delay_ms));
                return wait_interruptibly_ms(delay_ms);
            });
        if (_stop.load() || result.status == sl_bus::FetchResultStatus::Cancelled) {
            return;
        }

        const auto completed_ms = monotonic_ms();
        if (result.status == sl_bus::FetchResultStatus::Success) {
            ESP_LOGI(TAG, "fetch success stop=%.*s departures=%u notices=%u",
                     static_cast<int>(stop.name.size()), stop.name.data(),
                     static_cast<unsigned>(result.data.departure_count),
                     static_cast<unsigned>(result.data.notice_count));
            _store.publish_success(stop_index, result.data, completed_ms);
            _schedule.record_success(stop_index, completed_ms);
            continue;
        }

        const bool cooldown_was_active = _schedule.cooldown_active(completed_ms);
        _schedule.record_failure(stop_index, result, completed_ms);
        if (!cooldown_was_active && _schedule.cooldown_active(completed_ms)) {
            publish_cooldown_state(stop_index);
        }
        ESP_LOGE(TAG, "fetch failed stop=%.*s kind=%s status=%d retry_after=%u: %s",
                 static_cast<int>(stop.name.size()), stop.name.data(),
                 failure_kind_text(result.failure_kind), result.http_status,
                 static_cast<unsigned>(result.retry_after_seconds), result.error.c_str());
        _store.publish_failure(stop_index, result.error.empty() ? "SL update failed" : result.error);
    }
}

bool AppSlBus::wait_interruptibly_ms(uint64_t duration_ms)
{
    const auto started_ms = monotonic_ms();
    while (!_stop.load()) {
        const auto elapsed_ms = monotonic_ms() - started_ms;
        if (elapsed_ms >= duration_ms) {
            return false;
        }
        const auto remaining_ms = duration_ms - elapsed_ms;
        const auto slice_ms = std::min<uint64_t>(remaining_ms, 1000);
        if (_wake && xSemaphoreTake(_wake, pdMS_TO_TICKS(static_cast<uint32_t>(slice_ms))) == pdTRUE) {
            if (_stop.load()) {
                return true;
            }
            continue;
        }
        GetHAL().feedTheDog();
    }
    return true;
}

void AppSlBus::request_home()
{
    if (_close_state.request_home() != sl_bus::SlBusCloseState::Action::Stop) {
        return;
    }

    _stop.store(true);
    _network_waiting.store(false);
    if (_wake) {
        xSemaphoreGive(_wake);
    }
    _drain_started_us = esp_timer_get_time();
    _drain_warning_logged = false;
    log_heap_boundary("Home requested; draining SL.BUS");
}

bool AppSlBus::poll_worker_completion()
{
    if (!_worker_started) {
        return true;
    }
    if (!_done || xSemaphoreTake(_done, 0) != pdTRUE) {
        return false;
    }

    // The semaphore has been consumed, so stop_worker() must not take it
    // again. The worker has released its local HTTP/parser resources before
    // giving _done and never touches app members afterward.
    _worker_started = false;
    return true;
}

void AppSlBus::publish_cooldown_state(std::size_t skip_stop)
{
    if (_busy_published) {
        return;
    }
    for (std::size_t stop_index = 0; stop_index < sl_bus::kStopCount; ++stop_index) {
        if (stop_index != skip_stop) {
            _store.publish_failure(stop_index, "SL busy; retry later");
        }
    }
    _busy_published = true;
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
