#include "app_gesture.h"
#include "gesture_gate.hpp"
#include "gesture_models.hpp"
#include <apps/common/home_indicator/home_indicator.h>
#include <apps/common/status_bar/status_bar.h>
#include <assets/assets.h>
#include <hal/hal.h>
#include <hal/board/hal_bridge.h>
#include <esp_heap_caps.h>
#include <esp_imgfx_color_convert.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <freertos/task.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

static constexpr const char *TAG = "Gesture";
static constexpr float MINIMUM_SCORE = 0.85f;
static constexpr int64_t DRAIN_WARNING_US = 5000000;

namespace {

void log_heap_boundary(const char *event)
{
    ESP_LOGI(TAG, "%s t=%llu heap=%u largest=%u", event,
             static_cast<unsigned long long>(esp_timer_get_time() / 1000),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
}

}  // namespace

AppGesture::AppGesture()
{
    setAppInfo().name = "GESTURE";
    // Reuse a factory icon for this first hardware prototype.
    static auto icon = assets::get_image("icon_app_center.bin");
    setAppInfo().icon = (void *)&icon;
    static uint32_t color = 0x33CC99;
    setAppInfo().userData = &color;
}

AppGesture::~AppGesture() { stopWorker(); }

void AppGesture::onOpen()
{
    // onClose() has already joined any previous worker before a new session
    // can be opened.  Do not reset these values while an old worker exists.
    _thumbs_up_pending.store(false);
    _transition.reset();
    _drain_started_us = 0;
    _drain_warning_logged = false;

    _preview_pixels = static_cast<uint8_t *>(heap_caps_aligned_alloc(
        16, 320 * 240 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    {
        LvglLockGuard lock;
        if (_preview_pixels) {
            std::memset(_preview_pixels, 0, 320 * 240 * 2);
            _preview_image.header.magic = LV_IMAGE_HEADER_MAGIC;
            _preview_image.header.cf = LV_COLOR_FORMAT_RGB565;
            _preview_image.header.w = 320;
            _preview_image.header.h = 240;
            _preview_image.header.stride = 320 * 2;
            _preview_image.data_size = 320 * 240 * 2;
            _preview_image.data = _preview_pixels;
            _preview = lv_image_create(lv_screen_active());
            lv_image_set_src(_preview, &_preview_image);
            lv_obj_center(_preview);

            // Keep detection graphics separate from the camera pixels. A new
            // preview frame must not erase the box while inference is running.
            _hand_box = lv_obj_create(_preview);
            lv_obj_remove_style_all(_hand_box);
            lv_obj_set_style_bg_opa(_hand_box, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(_hand_box, lv_color_hex(0x00FF00), 0);
            lv_obj_set_style_border_opa(_hand_box, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(_hand_box, 2, 0);
            lv_obj_set_style_radius(_hand_box, 0, 0);
            lv_obj_remove_flag(_hand_box, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(_hand_box, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(_hand_box, LV_OBJ_FLAG_HIDDEN);

            _status = lv_label_create(lv_screen_active());
            lv_obj_set_width(_status, 304);
            lv_obj_align(_status, LV_ALIGN_BOTTOM_MID, 0, -20);
            lv_obj_set_style_bg_color(_status, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(_status, LV_OPA_70, 0);
            lv_obj_set_style_text_color(_status, lv_color_white(), 0);
            lv_obj_set_style_pad_all(_status, 5, 0);
            lv_label_set_text(_status, "Loading local gesture models...");
        }
        view::create_home_indicator([this]() { onHomeRequested(); }, 0xFFFFFF, 0x184A3A);
        view::create_status_bar(0xFFFFFF, 0x184A3A);
    }
    if (!_preview_pixels) {
        log_heap_boundary("open without preview");
        return;
    }
    _stop = false;
    _results = xQueueCreate(1, sizeof(Result));
    _done = xSemaphoreCreateBinary();
    if (!_results || !_done) {
        stopWorker();
        LvglLockGuard lock;
        if (_status) lv_label_set_text(_status, "Not enough memory");
        log_heap_boundary("open worker resources failed");
        return;
    }
    _worker_started = xTaskCreatePinnedToCore(worker, "gesture", 16384, this,
                                              2, nullptr, 0) == pdPASS;
    if (!_worker_started) {
        report("Cannot start gesture detection");
        log_heap_boundary("open worker start failed");
    } else {
        log_heap_boundary("open worker started");
    }
}

void AppGesture::report(const char *message, bool confirmed)
{
    if (!_results) return;
    Result result{};
    std::snprintf(result.message, sizeof(result.message), "%s", message);
    result.confirmed = confirmed;
    xQueueOverwrite(_results, &result);
}

void AppGesture::onRunning()
{
    // Home processing can invoke the callback synchronously.  It only changes
    // the UI-owned transition state and the stop flag, so it is safe here and
    // must happen before consuming a same-tick confirmation.
    {
        LvglLockGuard lock;
        view::update_home_indicator();
    }

    const bool pending = _thumbs_up_pending.exchange(false);
    auto action = _transition.consumeConfirmation(pending, static_cast<bool>(onThumbsUpConfirmed));
    if (action == GestureTransition::Action::StopForSl) {
        _stop.store(true);
        _drain_started_us = esp_timer_get_time();
        _drain_warning_logged = false;
        log_heap_boundary("thumbs-up consumed; draining for SL.BUS");
    }

    const bool worker_done = pollWorkerCompletion();
    if (worker_done) {
        if (_transition.isDraining()) log_heap_boundary("gesture drain complete");
        const auto completion_action = _transition.workerCompleted();
        if (completion_action != GestureTransition::Action::None) action = completion_action;
    }

    if (_transition.isDraining() && _drain_started_us > 0 && !_drain_warning_logged &&
        esp_timer_get_time() - _drain_started_us > DRAIN_WARNING_US) {
        ESP_LOGE(TAG, "gesture drain exceeded 5 seconds; hardware validation FAILED until investigated");
        _drain_warning_logged = true;
    }

    {
        LvglLockGuard lock;
        Result result{};
        if (_results && xQueueReceive(_results, &result, 0) == pdTRUE && _status) {
            lv_label_set_text(_status, result.message);
            lv_obj_set_style_bg_color(_status,
                                      result.confirmed ? lv_color_hex(0x146B3A) : lv_color_black(), 0);
        }
        if (_status) {
            switch (_transition.state()) {
                case GestureTransition::State::StoppingForSl:
                    lv_label_set_text(_status, "Opening SL.BUS...");
                    lv_obj_set_style_bg_color(_status, lv_color_hex(0x146B3A), 0);
                    break;
                case GestureTransition::State::StoppingForHome:
                    lv_label_set_text(_status, "Returning to Launcher...");
                    lv_obj_set_style_bg_color(_status, lv_color_black(), 0);
                    break;
                default:
                    break;
            }
        }
        view::update_status_bar();
    }

    if (action == GestureTransition::Action::DispatchSl) {
        // The callback only queues a launcher transition.  It must run outside
        // the LVGL lock and before this app requests its own close.
        const bool accepted = onThumbsUpConfirmed && onThumbsUpConfirmed();
        if (!accepted) {
            ESP_LOGW(TAG, "Launcher rejected SL.BUS transition; returning to Launcher");
        } else {
            ESP_LOGI(TAG, "Launcher accepted SL.BUS transition");
        }
        close();
    } else if (action == GestureTransition::Action::Close) {
        close();
    }
}

void AppGesture::onHomeRequested()
{
    const auto action = _transition.requestHome();
    if (action != GestureTransition::Action::StopForHome) return;

    _thumbs_up_pending.store(false);
    _stop.store(true);
    _drain_started_us = esp_timer_get_time();
    _drain_warning_logged = false;
    log_heap_boundary("Home requested; draining gesture");
}

bool AppGesture::pollWorkerCompletion()
{
    if (!_worker_started) return true;
    if (!_done || xSemaphoreTake(_done, 0) != pdTRUE) return false;

    // The semaphore has been consumed, so stopWorker() must not take it again.
    _worker_started = false;
    return true;
}

void AppGesture::stopWorker()
{
    _stop = true;
    if (_worker_started) {
        // Finish capture/inference before another app can reuse the shared camera.
        if (_done) {
            while (xSemaphoreTake(_done, pdMS_TO_TICKS(50)) != pdTRUE) GetHAL().feedTheDog();
        }
        _worker_started = false;
    }
    if (_done) { vSemaphoreDelete(_done); _done = nullptr; }
    if (_results) { vQueueDelete(_results); _results = nullptr; }
}

void AppGesture::onClose()
{
    stopWorker();
    _thumbs_up_pending.store(false);
    _transition.reset();
    {
        LvglLockGuard lock;
        if (_status) { lv_obj_delete(_status); _status = nullptr; }
        if (_preview) { lv_obj_delete(_preview); _preview = nullptr; }
        _hand_box = nullptr; // Deleted with its preview parent.
        view::destroy_home_indicator();
        view::destroy_status_bar();
    }
    heap_caps_free(_preview_pixels);
    _preview_pixels = nullptr;
    log_heap_boundary("Gesture close complete");
}

void AppGesture::worker(void *context)
{
    auto *app = static_cast<AppGesture *>(context);
    try {
        app->recognize();
    } catch (const std::bad_alloc &) {
        app->report("Not enough memory for models");
    }
    // No app members may be accessed after signaling completion.
    const auto done = app->_done;
    xSemaphoreGive(done);
    vTaskDelete(nullptr);
}

void AppGesture::recognize()
{
    for (const char *name : {"hand_det", "hand_gesture_cls"}) {
        const auto *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                    ESP_PARTITION_SUBTYPE_ANY, name);
        char header[4]{};
        if (!part || esp_partition_read(part, 0, &header, sizeof(header)) != ESP_OK ||
            (std::memcmp(header, "EDL1", 4) != 0 && std::memcmp(header, "EDL2", 4) != 0 &&
             std::memcmp(header, "PDL1", 4) != 0 && std::memcmp(header, "PDL2", 4) != 0)) {
            report("Gesture models not installed");
            return;
        }
    }
    auto *camera = hal_bridge::board_get_camera();
    if (!camera) { report("Camera unavailable"); return; }

    struct Resources {
        uint8_t *rgb = nullptr;
        esp_imgfx_color_convert_handle_t converter = nullptr;
        ~Resources() {
            if (converter) esp_imgfx_color_convert_close(converter);
            heap_caps_free(rgb);
        }
    } resources;
    resources.rgb = static_cast<uint8_t *>(heap_caps_aligned_alloc(
        32, 320 * 240 * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!resources.rgb) { report("Not enough camera memory"); return; }

    // Load both models before accepting gestures, so the first hand does not
    // pay the classifier's lazy-loading cost.
    GestureDetector detector;
    GestureClassifier classifier;
    report("Show one thumbs-up");
    dl::image::img_t image{};
    image.data = resources.rgb;
    image.width = 320;
    image.height = 240;
    image.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;
    GestureGate gate;
    unsigned count = 0;
    int64_t next_timing_log = 0;
    int camera_format = 0;
    while (!_stop.load()) {
        const int64_t frame_started = esp_timer_get_time();
        if (!camera->StreamCaptures(true)) { report("Camera capture failed"); return; }
        if (_stop.load()) return;
        if (camera->GetFrameWidth() != 320 || camera->GetFrameHeight() != 240 ||
            !camera->GetFrameData()) {
            report("Unexpected camera image");
            return;
        }
        if (!resources.converter) {
            camera_format = camera->GetFrameFormat();
            esp_imgfx_color_convert_cfg_t config{};
            config.in_res = {320, 240};
            config.in_pixel_fmt = static_cast<esp_imgfx_pixel_fmt_t>(camera_format);
            config.out_pixel_fmt = ESP_IMGFX_PIXEL_FMT_RGB888;
            config.color_space_std = ESP_IMGFX_COLOR_SPACE_STD_BT601;
            if (esp_imgfx_color_convert_open(&config, &resources.converter) != ESP_IMGFX_ERR_OK) {
                report("Unsupported camera format");
                return;
            }
        }
        if (camera->GetFrameFormat() != camera_format) {
            report("Camera format changed");
            return;
        }
        const int64_t captured = esp_timer_get_time();
        esp_imgfx_data_t input{const_cast<uint8_t *>(camera->GetFrameData()),
                               static_cast<uint32_t>(camera->GetFrameSize())};
        esp_imgfx_data_t output{resources.rgb, 320 * 240 * 3};
        if (esp_imgfx_color_convert_process(resources.converter, &input, &output) != ESP_IMGFX_ERR_OK) {
            report("Image conversion failed");
            return;
        }
        {
            // LVGL must not read pixels while the worker writes them. Publish the
            // new camera image now, before either model blocks this worker.
            LvglLockGuard lock;
            for (size_t pixel = 0; pixel < 320 * 240; ++pixel) {
                const uint8_t red = resources.rgb[pixel * 3];
                const uint8_t green = resources.rgb[pixel * 3 + 1];
                const uint8_t blue = resources.rgb[pixel * 3 + 2];
                const uint16_t rgb565 = static_cast<uint16_t>(((red & 0xF8) << 8) |
                                                              ((green & 0xFC) << 3) | (blue >> 3));
                _preview_pixels[pixel * 2] = static_cast<uint8_t>(rgb565);
                _preview_pixels[pixel * 2 + 1] = static_cast<uint8_t>(rgb565 >> 8);
            }
            lv_obj_invalidate(_preview);
        }
        const int64_t preview_ready = esp_timer_get_time();
        // Use the same converted image for preview and recognition.
        const auto &inference_image = image;
        auto &hands = detector.run(inference_image);
        const int64_t detected = esp_timer_get_time();
        if (_stop.load()) return;
        {
            LvglLockGuard lock;
            bool visible = false;
            if (!hands.empty() && hands.front().box.size() >= 4) {
                const auto &box = hands.front().box;
                const int left = std::clamp(box[0], 0, 319);
                const int top = std::clamp(box[1], 0, 239);
                const int right = std::clamp(box[2], 0, 319);
                const int bottom = std::clamp(box[3], 0, 239);
                if (right > left && bottom > top) {
                    lv_obj_set_pos(_hand_box, left, top);
                    lv_obj_set_size(_hand_box, right - left + 1, bottom - top + 1);
                    visible = true;
                }
            }
            if (visible) lv_obj_remove_flag(_hand_box, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(_hand_box, LV_OBJ_FLAG_HIDDEN);
        }
        bool thumbs_up = false;
        const char *gesture = "none";
        float score = 0.0f;
        const int64_t classification_started = esp_timer_get_time();
        // While latched, only the detector is needed to observe hand removal.
        const bool classify = hands.size() == 1 && hands.front().box.size() >= 4 && !gate.latched();
        if (classify) {
            const auto &box = hands.front().box;
            const auto results = classifier.run_crop(inference_image, {box[0], box[1], box[2], box[3]});
            if (results.size() == 1) {
                const auto &r = results.front();
                gesture = r.cat_name ? r.cat_name : "unknown";
                score = std::isfinite(r.score) ? r.score : 0.0f;
                thumbs_up = r.cat_name && std::strcmp(r.cat_name, "like") == 0 &&
                            std::isfinite(r.score) && r.score >= MINIMUM_SCORE;
            }
        }
        const int64_t classified = esp_timer_get_time();
        if (_stop.load()) return;
        if (gate.update(esp_timer_get_time() / 1000, thumbs_up, hands.empty())) {
            ESP_LOGI(TAG, "THUMBS_UP_CONFIRMED %u", ++count);
            // This is a one-bit event mailbox, separate from the presentation
            // queue below.  The UI thread consumes it with exchange(false).
            _thumbs_up_pending.store(true);
        }
        if (gate.latched()) report("Thumbs-up received! Lower your hand", true);
        else {
            char status[64];
            const long elapsed_ms = static_cast<long>((esp_timer_get_time() - frame_started) / 1000);
            if (hands.empty()) {
                std::snprintf(status, sizeof(status), "No hand | %ld ms", elapsed_ms);
            } else if (hands.size() != 1) {
                std::snprintf(status, sizeof(status), "%u hands | best %.0f%% | %ld ms",
                              static_cast<unsigned>(hands.size()), hands.front().score * 100.0f,
                              elapsed_ms);
            } else {
                std::snprintf(status, sizeof(status), "%s %.0f%% | %ld ms%s", gesture,
                              score * 100.0f, elapsed_ms, thumbs_up ? " | HOLD" : "");
            }
            report(status);
        }
        const int64_t finished = esp_timer_get_time();
        if (finished >= next_timing_log) {
            ESP_LOGI(TAG, "Timing ms: capture=%ld preview=%ld detect=%ld classify=%ld total=%ld "
                          "hands=%u cls=%d heap=%u",
                     static_cast<long>((captured - frame_started) / 1000),
                     static_cast<long>((preview_ready - captured) / 1000),
                     static_cast<long>((detected - preview_ready) / 1000),
                     static_cast<long>((classified - classification_started) / 1000),
                     static_cast<long>((finished - frame_started) / 1000),
                     static_cast<unsigned>(hands.size()), classify,
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
            next_timing_log = finished + 5000000;
        }
        vTaskDelay(1); // Yield to the UI/watchdog without a fixed 30 ms pause.
    }
}
