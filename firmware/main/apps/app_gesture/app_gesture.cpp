#include "app_gesture.h"
#include "gesture_gate.hpp"
#include <apps/common/common.h>
#include <assets/assets.h>
#include <hal/hal.h>
#include <hal/board/hal_bridge.h>
#include <stackchan/stackchan.h>
#include <hand_detect.hpp>
#include <hand_gesture_recognition.hpp>
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

using namespace stackchan;
static constexpr const char *TAG = "Gesture";
static constexpr float MINIMUM_HAND_SCORE = 0.60f;
static constexpr float MINIMUM_SCORE = 0.85f;

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

            _status = lv_label_create(lv_screen_active());
            lv_obj_set_width(_status, 304);
            lv_obj_align(_status, LV_ALIGN_BOTTOM_MID, 0, -20);
            lv_obj_set_style_bg_color(_status, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(_status, LV_OPA_70, 0);
            lv_obj_set_style_text_color(_status, lv_color_white(), 0);
            lv_obj_set_style_pad_all(_status, 5, 0);
            lv_label_set_text(_status, "Loading local gesture models...");
        }
        view::create_home_indicator([this]() { close(); }, 0xFFFFFF, 0x184A3A);
        view::create_status_bar(0xFFFFFF, 0x184A3A);
    }
    if (!_preview_pixels) return;
    _stop = false;
    _results = xQueueCreate(1, sizeof(Result));
    _done = xSemaphoreCreateBinary();
    if (!_results || !_done) {
        stopWorker();
        LvglLockGuard lock;
        GetStackChan().avatar().setSpeech("Not enough memory");
        return;
    }
    _worker_started = xTaskCreatePinnedToCore(worker, "gesture", 16384, this,
                                              2, nullptr, 0) == pdPASS;
    if (!_worker_started) report("Cannot start gesture detection");
}

void AppGesture::report(const char *message, bool confirmed)
{
    Result result{};
    std::snprintf(result.message, sizeof(result.message), "%s", message);
    result.confirmed = confirmed;
    xQueueOverwrite(_results, &result);
}

void AppGesture::onRunning()
{
    LvglLockGuard lock;
    Result result{};
    if (_results && xQueueReceive(_results, &result, 0) == pdTRUE) {
        if (_status) {
            lv_label_set_text(_status, result.message);
            lv_obj_set_style_bg_color(_status,
                                      result.confirmed ? lv_color_hex(0x146B3A) : lv_color_black(), 0);
        }
    }
    const unsigned revision = _preview_revision.load();
    if (_preview && revision != _displayed_preview_revision) {
        _displayed_preview_revision = revision;
        lv_obj_invalidate(_preview);
    }
    view::update_home_indicator();
    view::update_status_bar();
}

void AppGesture::stopWorker()
{
    _stop = true;
    if (_worker_started) {
        // Finish capture/inference before another app can reuse the shared camera.
        while (xSemaphoreTake(_done, pdMS_TO_TICKS(50)) != pdTRUE) GetHAL().feedTheDog();
        _worker_started = false;
    }
    if (_done) { vSemaphoreDelete(_done); _done = nullptr; }
    if (_results) { vQueueDelete(_results); _results = nullptr; }
}

void AppGesture::onClose()
{
    stopWorker();
    {
        LvglLockGuard lock;
        if (_status) { lv_obj_delete(_status); _status = nullptr; }
        if (_preview) { lv_obj_delete(_preview); _preview = nullptr; }
        view::destroy_home_indicator();
        view::destroy_status_bar();
    }
    heap_caps_free(_preview_pixels);
    _preview_pixels = nullptr;
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

    HandDetect detector;
    detector.set_score_thr(MINIMUM_HAND_SCORE);
    HandGestureRecognizer recognizer(HandGestureCls::MOBILENETV2_0_5_S8_V1);
    dl::image::img_t image{};
    image.data = resources.rgb;
    image.width = 320;
    image.height = 240;
    image.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;
    GestureGate gate;
    unsigned count = 0;
    int camera_format = 0;
    while (!_stop.load()) {
        const int64_t frame_started = esp_timer_get_time();
        if (!camera->StreamCaptures()) { report("Camera capture failed"); return; }
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
        esp_imgfx_data_t input{const_cast<uint8_t *>(camera->GetFrameData()),
                               static_cast<uint32_t>(camera->GetFrameSize())};
        esp_imgfx_data_t output{resources.rgb, 320 * 240 * 3};
        if (esp_imgfx_color_convert_process(resources.converter, &input, &output) != ESP_IMGFX_ERR_OK) {
            report("Image conversion failed");
            return;
        }
        for (size_t pixel = 0; pixel < 320 * 240; ++pixel) {
            const uint8_t red = resources.rgb[pixel * 3];
            const uint8_t green = resources.rgb[pixel * 3 + 1];
            const uint8_t blue = resources.rgb[pixel * 3 + 2];
            const uint16_t rgb565 = static_cast<uint16_t>(((red & 0xF8) << 8) |
                                                          ((green & 0xFC) << 3) | (blue >> 3));
            _preview_pixels[pixel * 2] = static_cast<uint8_t>(rgb565);
            _preview_pixels[pixel * 2 + 1] = static_cast<uint8_t>(rgb565 >> 8);
        }
        dl::image::img_t inference_image = image;
        if (camera_format == ESP_IMGFX_PIXEL_FMT_YUYV) {
            inference_image.data = const_cast<uint8_t *>(camera->GetFrameData());
            inference_image.pix_type = dl::image::DL_IMAGE_PIX_TYPE_YUYV;
        }
        auto &hands = detector.run(inference_image);
        if (!hands.empty() && hands.front().box.size() >= 4) {
            const auto &box = hands.front().box;
            const int left = std::max(0, std::min(319, box[0]));
            const int top = std::max(0, std::min(239, box[1]));
            const int right = std::max(0, std::min(319, box[2]));
            const int bottom = std::max(0, std::min(239, box[3]));
            for (int x = left; x <= right; ++x) {
                for (int y : {top, std::min(top + 1, 239), std::max(bottom - 1, 0), bottom}) {
                    _preview_pixels[(y * 320 + x) * 2] = 0xE0;
                    _preview_pixels[(y * 320 + x) * 2 + 1] = 0x07;
                }
            }
            for (int y = top; y <= bottom; ++y) {
                for (int x : {left, std::min(left + 1, 319), std::max(right - 1, 0), right}) {
                    _preview_pixels[(y * 320 + x) * 2] = 0xE0;
                    _preview_pixels[(y * 320 + x) * 2 + 1] = 0x07;
                }
            }
        }
        _preview_revision.fetch_add(1);
        bool thumbs_up = false;
        const char *gesture = "none";
        float score = 0.0f;
        if (hands.size() == 1) {
            const auto results = recognizer.recognize(inference_image, hands);
            if (results.size() == 1) {
                const auto &r = results.front();
                gesture = r.cat_name ? r.cat_name : "unknown";
                score = std::isfinite(r.score) ? r.score : 0.0f;
                thumbs_up = r.cat_name && std::strcmp(r.cat_name, "like") == 0 &&
                            std::isfinite(r.score) && r.score >= MINIMUM_SCORE;
                ESP_LOGI(TAG, "%s: %.2f", r.cat_name ? r.cat_name : "unknown", r.score);
            }
        }
        if (_stop.load()) return;
        if (gate.update(esp_timer_get_time() / 1000, thumbs_up, hands.empty())) {
            ESP_LOGI(TAG, "THUMBS_UP_CONFIRMED %u", ++count);
            // A future SL request can be queued here without blocking capture.
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
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
