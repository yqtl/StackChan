/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <atomic>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class FocusChime {
public:
    enum class Result : uint8_t { None, Played, Cancelled, Failed };

    FocusChime();
    ~FocusChime();
    FocusChime(const FocusChime&) = delete;
    FocusChime& operator=(const FocusChime&) = delete;

    bool start();
    void request_cancel();
    bool poll_finished(Result* result = nullptr);
    bool active() const { return _active; }
    void shutdown();

private:
    static void worker(void* context);
    Result play();

    SemaphoreHandle_t _done = nullptr;
    std::atomic<bool> _cancel_requested{false};
    std::atomic<Result> _result{Result::None};
    bool _active = false;  // Owned by the app update thread.
};
