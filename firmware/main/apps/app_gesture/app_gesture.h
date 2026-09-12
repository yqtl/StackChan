#pragma once
#include <atomic>
#include <mooncake.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <lvgl.h>

class AppGesture : public mooncake::AppAbility {
public:
    AppGesture();
    ~AppGesture() override;
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
    void stopWorker();
    std::atomic<bool> _stop{false};
    std::atomic<unsigned> _preview_revision{0};
    QueueHandle_t _results = nullptr;
    SemaphoreHandle_t _done = nullptr;
    uint8_t *_preview_pixels = nullptr;
    lv_image_dsc_t _preview_image{};
    lv_obj_t *_preview = nullptr;
    lv_obj_t *_status = nullptr;
    unsigned _displayed_preview_revision = 0;
    bool _worker_started = false;
};
