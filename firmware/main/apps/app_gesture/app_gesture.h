#pragma once
#include "gesture_transition.hpp"

#include <atomic>
#include <functional>
#include <mooncake.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <lvgl.h>

class AppGesture : public mooncake::AppAbility {
public:
    AppGesture();
    ~AppGesture() override;

    // Set during startup, while the app is not running.  The callback is
    // invoked on the Mooncake/UI thread only after the worker has drained.
    std::function<bool()> onThumbsUpConfirmed;

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    struct Result {
        char message[64];
        bool confirmed;
    };
    static void worker(void *context);
    void recognize();
    void report(const char *message, bool confirmed = false);
    void onHomeRequested();
    bool pollWorkerCompletion();
    void stopWorker();

    std::atomic<bool> _thumbs_up_pending{false};
    std::atomic<bool> _stop{false};
    QueueHandle_t _results = nullptr;
    SemaphoreHandle_t _done = nullptr;
    uint8_t *_preview_pixels = nullptr;
    lv_image_dsc_t _preview_image{};
    lv_obj_t *_preview = nullptr;
    lv_obj_t *_hand_box = nullptr;
    lv_obj_t *_status = nullptr;
    bool _worker_started = false;
    GestureTransition _transition;
    int64_t _drain_started_us = 0;
    bool _drain_warning_logged = false;
};
