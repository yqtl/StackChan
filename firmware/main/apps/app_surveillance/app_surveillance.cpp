#include "app_surveillance.h"
#include "queue_budget.hpp"

#include <apps/common/home_indicator/home_indicator.h>
#include <apps/common/status_bar/status_bar.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <hal/board/hal_bridge.h>
#include <hal/board/stackchan_camera.h>
#include <hal/hal.h>
#include <image_to_jpeg.h>
#include <linux/videodev2.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <settings.h>
#include <stackchan/stackchan.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <strings.h>
#include <utility>
#include <unistd.h>

#if __has_include("connection_profile.h")
#include "connection_profile.h"
#else
#define SURVEILLANCE_PROFILE_CONFIGURED 0
static constexpr const char kSurveillanceReceiverHost[] = "stackchan-surveillance.local";
static constexpr int kSurveillanceReceiverPort = 8443;
static constexpr const char kSurveillancePairingToken[] = "";
static constexpr const char kSurveillanceCaPem[] = "";
#endif

namespace {

constexpr const char* TAG = "Surveillance";
constexpr const char* kSettingsNamespace = "surveillance";
constexpr std::size_t kMaxPendingImages = surveillance::kMaxPendingImages;
constexpr std::size_t kMaxPendingBytes = surveillance::kMaxPendingBytes;
constexpr std::uint32_t kCaptureIntervalMs = 500;
constexpr std::uint32_t kHeartbeatIntervalMs = 5000;
constexpr std::uint32_t kHttpTimeoutMs = 5000;
constexpr int kServoSpeed = 200;

struct HttpResponseBuffer {
    char data[320]{};
    std::size_t size = 0;
};

esp_err_t http_event(esp_http_client_event_t* event)
{
    if (event && event->event_id == HTTP_EVENT_ON_DATA && event->user_data && event->data && event->data_len > 0) {
        auto* response = static_cast<HttpResponseBuffer*>(event->user_data);
        const std::size_t available = sizeof(response->data) - 1 - response->size;
        const std::size_t amount = std::min(available, static_cast<std::size_t>(event->data_len));
        std::memcpy(response->data + response->size, event->data, amount);
        response->size += amount;
        response->data[response->size] = '\0';
    }
    return ESP_OK;
}

void escape_json(const char* input, char* output, std::size_t capacity)
{
    std::size_t written = 0;
    for (const char* cursor = input; cursor && *cursor && written + 2 < capacity; ++cursor) {
        if (*cursor == '"' || *cursor == '\\') output[written++] = '\\';
        if (static_cast<unsigned char>(*cursor) >= 0x20 && written + 1 < capacity) output[written++] = *cursor;
    }
    if (capacity > 0) output[written] = '\0';
}

bool to_gray_80x60(const std::uint8_t* frame, std::size_t frame_size, int width, int height,
                   int format, std::array<std::uint8_t, surveillance::kMotionPixels>& output)
{
    if (!frame || width != 320 || height != 240) return false;
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    if (format == V4L2_PIX_FMT_GREY && frame_size < pixels) return false;
    if (format == V4L2_PIX_FMT_RGB565 && frame_size < pixels * 2) return false;
    if (format == V4L2_PIX_FMT_RGB24 && frame_size < pixels * 3) return false;
    if (format == V4L2_PIX_FMT_YUYV && frame_size < pixels * 2) return false;
    if (format == V4L2_PIX_FMT_YUV420 && frame_size < pixels * 3 / 2) return false;

    for (std::size_t y = 0; y < surveillance::kMotionHeight; ++y) {
        const std::size_t source_y = y * 4;
        for (std::size_t x = 0; x < surveillance::kMotionWidth; ++x) {
            const std::size_t source_x = x * 4;
            const std::size_t index = source_y * static_cast<std::size_t>(width) + source_x;
            std::uint8_t gray = 0;
            if (format == V4L2_PIX_FMT_GREY) {
                gray = frame[index];
            } else if (format == V4L2_PIX_FMT_RGB565) {
                const std::uint16_t value = static_cast<std::uint16_t>(frame[index * 2]) |
                                            (static_cast<std::uint16_t>(frame[index * 2 + 1]) << 8);
                const int red = ((value >> 11) & 0x1F) * 255 / 31;
                const int green = ((value >> 5) & 0x3F) * 255 / 63;
                const int blue = (value & 0x1F) * 255 / 31;
                gray = static_cast<std::uint8_t>((77 * red + 150 * green + 29 * blue) >> 8);
            } else if (format == V4L2_PIX_FMT_RGB24) {
                const int red = frame[index * 3];
                const int green = frame[index * 3 + 1];
                const int blue = frame[index * 3 + 2];
                gray = static_cast<std::uint8_t>((77 * red + 150 * green + 29 * blue) >> 8);
            } else if (format == V4L2_PIX_FMT_YUYV) {
                const std::size_t pair = (source_y * static_cast<std::size_t>(width) + (source_x & ~std::size_t{1})) * 2;
                gray = frame[pair + ((source_x & 1) ? 2 : 0)];
            } else if (format == V4L2_PIX_FMT_YUV420) {
                gray = frame[index];
            } else {
                return false;
            }
            output[y * surveillance::kMotionWidth + x] = gray;
        }
    }
    return true;
}

void make_short_id(char output[33])
{
    const std::uint64_t monotonic = static_cast<std::uint64_t>(esp_timer_get_time());
    const std::uint64_t random = (static_cast<std::uint64_t>(esp_random()) << 32) | esp_random();
    std::snprintf(output, 33, "%016llx%016llx",
                  static_cast<unsigned long long>(monotonic),
                  static_cast<unsigned long long>(random));
}

std::uint64_t monotonic_ms()
{
    return static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
}

int absolute(int value) { return value < 0 ? -value : value; }

void write_u16(std::uint8_t* destination, std::uint16_t value)
{
    destination[0] = static_cast<std::uint8_t>(value >> 8);
    destination[1] = static_cast<std::uint8_t>(value);
}

std::uint16_t read_u16(const std::uint8_t* source)
{
    return static_cast<std::uint16_t>((source[0] << 8) | source[1]);
}

bool read_dns_name(const std::uint8_t* packet, std::size_t packet_size,
                   std::size_t& offset, char* name, std::size_t name_capacity)
{
    std::size_t cursor = offset;
    std::size_t output_size = 0;
    std::size_t next_offset = offset;
    bool jumped = false;
    unsigned jumps = 0;
    while (cursor < packet_size) {
        const std::uint8_t label = packet[cursor];
        if (label == 0) {
            if (!jumped) next_offset = cursor + 1;
            if (output_size > 0) --output_size;
            if (output_size >= name_capacity) return false;
            name[output_size] = '\0';
            offset = next_offset;
            return true;
        }
        if ((label & 0xC0) == 0xC0) {
            if (cursor + 1 >= packet_size || ++jumps > 16) return false;
            const std::size_t target = ((label & 0x3F) << 8) | packet[cursor + 1];
            if (target >= packet_size) return false;
            if (!jumped) next_offset = cursor + 2;
            jumped = true;
            cursor = target;
            continue;
        }
        if ((label & 0xC0) != 0 || label > 63 || cursor + 1 + label > packet_size) return false;
        if (output_size + label + 1 >= name_capacity) return false;
        std::memcpy(name + output_size, packet + cursor + 1, label);
        output_size += label;
        name[output_size++] = '.';
        cursor += label + 1;
    }
    return false;
}

bool resolve_mdns_ipv4(const char* configured_host, char output[16])
{
    if (!configured_host || !configured_host[0]) return false;
    std::string query_name(configured_host);
    if (!query_name.empty() && query_name.back() == '.') query_name.pop_back();
    if (query_name.size() < 6 || query_name.compare(query_name.size() - 6, 6, ".local") != 0)
        query_name += ".local";
    if (query_name.size() > 240) return false;

    std::uint8_t query[512]{};
    write_u16(query, 0);
    write_u16(query + 2, 0);
    write_u16(query + 4, 1);
    std::size_t cursor = 12;
    std::size_t start = 0;
    while (start < query_name.size()) {
        const auto separator = query_name.find('.', start);
        const auto end = separator == std::string::npos ? query_name.size() : separator;
        const auto label_size = end - start;
        if (label_size == 0 || label_size > 63 || cursor + label_size + 1 >= sizeof(query)) return false;
        query[cursor++] = static_cast<std::uint8_t>(label_size);
        std::memcpy(query + cursor, query_name.data() + start, label_size);
        cursor += label_size;
        if (separator == std::string::npos) break;
        start = separator + 1;
    }
    query[cursor++] = 0;
    write_u16(query + cursor, 1); cursor += 2;       // A record
    write_u16(query + cursor, 0x8001); cursor += 2;  // IN class, request unicast response

    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return false;
    timeval timeout{};
    timeout.tv_usec = 300'000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    int ttl = 255;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(5353);
    inet_aton("224.0.0.251", &destination.sin_addr);

    bool resolved = false;
    for (unsigned attempt = 0; attempt < 7 && !resolved; ++attempt) {
        sendto(fd, query, cursor, 0, reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
        std::uint8_t response[1500];
        const int received = recvfrom(fd, response, sizeof(response), 0, nullptr, nullptr);
        if (received < 12 || (read_u16(response + 2) & 0x8000) == 0) continue;
        const std::size_t questions = read_u16(response + 4);
        const std::size_t records = read_u16(response + 6) + read_u16(response + 8) + read_u16(response + 10);
        std::size_t offset = 12;
        char owner[256];
        bool valid = true;
        for (std::size_t i = 0; i < questions; ++i) {
            if (!read_dns_name(response, received, offset, owner, sizeof(owner)) || offset + 4 > static_cast<std::size_t>(received)) {
                valid = false; break;
            }
            offset += 4;
        }
        for (std::size_t i = 0; valid && i < records; ++i) {
            if (!read_dns_name(response, received, offset, owner, sizeof(owner)) || offset + 10 > static_cast<std::size_t>(received)) {
                valid = false; break;
            }
            const auto type = read_u16(response + offset);
            const auto record_class = read_u16(response + offset + 2) & 0x7FFF;
            const auto data_size = read_u16(response + offset + 8);
            offset += 10;
            if (offset + data_size > static_cast<std::size_t>(received)) { valid = false; break; }
            if (type == 1 && record_class == 1 && data_size == 4 && strcasecmp(owner, query_name.c_str()) == 0) {
                in_addr address{};
                std::memcpy(&address, response + offset, 4);
                resolved = inet_ntop(AF_INET, &address, output, 16) != nullptr;
                break;
            }
            offset += data_size;
        }
    }
    close(fd);
    return resolved;
}

}  // namespace

AppSurveillance::AppSurveillance()
{
    setAppInfo().name = "SURVEILLANCE";
    static std::uint32_t theme_color = 0x35B7A4;
    setAppInfo().userData = &theme_color;
}

AppSurveillance::~AppSurveillance()
{
    stop_workers();
}

void AppSurveillance::onCreate()
{
    ESP_LOGI(TAG, "created");
}

void AppSurveillance::onOpen()
{
    load_settings();
    _armed.store(false);  // Opening or rebooting never rearms surveillance.
    _stop_capture.store(false);
    _stop_upload.store(false);
    _suspend_detection.store(true);
    _burst_active.store(false);
    _reset_baseline.store(true);
    _detector.reset();
    _receiver_connected.store(false);
    _pending_count.store(0);
    _pending_bytes.store(0);
    _clock_anchor_receiver_ms.store(0);
    _clock_anchor_monotonic_ms.store(0);
    _last_heartbeat_ms.store(0);
    _last_feedback_ms = 0;
    _home_closing = false;
    _home_requested.store(false);
    _heartbeat_requested.store(false);
    _scan_active = false;
    _scan_capture_queued = false;
    _test_sweep_completed = false;
    _setup_page = false;
    make_id(_boot_id);
    {
        std::lock_guard<std::mutex> lock(_status_mutex);
        _error[0] = '\0';
        _last_saved_capture_id[0] = '\0';
        _last_scan_group_id[0] = '\0';
        std::snprintf(_scan_record_state, sizeof(_scan_record_state), "idle");
        _scan_record_message[0] = '\0';
    }

    {
        LvglLockGuard lock;
        create_ui();
        view::create_home_indicator([this]() { _home_requested = true; }, 0xFFFFFF, 0x124C49);
        view::create_status_bar(0xFFFFFF, 0x124C49);
    }

    _uploads = xQueueCreate(kMaxPendingImages, sizeof(UploadImage*));
    _preview_frames = xQueueCreate(1, sizeof(PreviewFrame));
    _scan_requests = xQueueCreate(1, sizeof(CaptureRequest));
    _scan_results = xQueueCreate(1, sizeof(ScanCaptureResult));
    _queue_guard = xSemaphoreCreateMutex();
    _capture_done = xSemaphoreCreateBinary();
    _upload_done = xSemaphoreCreateBinary();
    _resources_ready = _uploads && _preview_frames && _scan_requests && _scan_results &&
                       _queue_guard && _capture_done && _upload_done;
    if (!_resources_ready) {
        update_status("Not enough memory for surveillance workers");
        stop_workers();
        return;
    }

    if (SURVEILLANCE_PROFILE_CONFIGURED) {
        update_status("Disarmed · receiver connecting");
    } else {
        update_status("Disarmed · pair receiver over USB first");
    }

    _capture_started = xTaskCreatePinnedToCore(capture_worker, "surv_camera", 12288, this, 2,
                                                &_capture_task, 0) == pdPASS;
    _upload_started = xTaskCreatePinnedToCore(upload_worker, "surv_upload", 9216, this, 2,
                                               &_upload_task, 0) == pdPASS;
    if (!_capture_started || !_upload_started) {
        _stop_capture.store(true);
        _stop_upload.store(true);
        update_status("Could not start surveillance workers");
        stop_workers();
    }
    ESP_LOGI(TAG, "opened disarmed; profile=%s", SURVEILLANCE_PROFILE_CONFIGURED ? "paired" : "missing");
}

void AppSurveillance::onRunning()
{
    {
        LvglLockGuard lock;
        view::update_home_indicator();
        view::update_status_bar();
    }

    if (_home_requested && !_home_closing) begin_home_close();
    const ButtonAction pending = _pending_action.exchange(ButtonAction::None);
    if (!_home_closing && pending != ButtonAction::None) handle_button(pending);
    GetStackChan().motion().update();

    if (!_home_closing) {
        ScanCaptureResult result{};
        if (_scan_results && xQueueReceive(_scan_results, &result, 0) == pdTRUE && _scan_capture_queued) {
            _scan_capture_queued = false;
            handle_scan_action(_scan.capture_completed(monotonic_ms(), result.saved_to_ram));
        }

        if (_scan_active) {
            surveillance::ServoFeedback feedback;
            const auto action = _scan.update(monotonic_ms(), read_servo_feedback(feedback) ? feedback : surveillance::ServoFeedback{});
            handle_scan_action(action);
        } else if (_armed.load()) {
            const auto now = monotonic_ms();
            if (now - _last_feedback_ms >= 500) {
                _last_feedback_ms = now;
                surveillance::ServoFeedback feedback;
                if (!read_servo_feedback(feedback) ||
                    surveillance::position_drifted(feedback, _watch_yaw, _watch_pitch)) {
                    fail_scan("servo drift or feedback loss; reposition before rearming");
                }
            }
            if (!_scan_active && _schedule.due(now) && !_burst_active.load()) start_scan(false);
        }
    }

    update_ui();
    if (poll_worker_completion() && _home_closing) close();
}

void AppSurveillance::onClose()
{
    _armed.store(false);
    _stop_capture.store(true);
    _stop_upload.store(true);
    GetStackChan().motion().cancelAndRelease();
    stop_workers();

    {
        LvglLockGuard lock;
        view::destroy_home_indicator();
        view::destroy_status_bar();
        for (lv_obj_t*& button : _main_buttons) {
            if (button) lv_obj_delete(button);
            button = nullptr;
        }
        for (lv_obj_t*& button : _setup_buttons) {
            if (button) lv_obj_delete(button);
            button = nullptr;
        }
        if (_preview) lv_obj_delete(_preview);
        if (_status_label) lv_obj_delete(_status_label);
        if (_queue_label) lv_obj_delete(_queue_label);
        _preview = nullptr;
        _status_label = nullptr;
        _queue_label = nullptr;
        _button_contexts.clear();
    }
    heap_caps_free(_preview_pixels);
    _preview_pixels = nullptr;
    Settings settings(kSettingsNamespace, true);
    settings.SetInt("lost_images", static_cast<int>(_lost_images.load()));
}

void AppSurveillance::create_ui()
{
    _preview_pixels = static_cast<std::uint8_t*>(heap_caps_aligned_alloc(
        16, surveillance::kMotionPixels * sizeof(std::uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (_preview_pixels) {
        std::memset(_preview_pixels, 0, surveillance::kMotionPixels * sizeof(std::uint16_t));
        _preview_image.header.magic = LV_IMAGE_HEADER_MAGIC;
        _preview_image.header.cf = LV_COLOR_FORMAT_RGB565;
        _preview_image.header.w = surveillance::kMotionWidth;
        _preview_image.header.h = surveillance::kMotionHeight;
        _preview_image.header.stride = surveillance::kMotionWidth * sizeof(std::uint16_t);
        _preview_image.data_size = surveillance::kMotionPixels * sizeof(std::uint16_t);
        _preview_image.data = _preview_pixels;
        _preview = lv_image_create(lv_screen_active());
        lv_image_set_src(_preview, &_preview_image);
        lv_obj_set_pos(_preview, 120, 34);
        lv_image_set_scale(_preview, 512);
    }

    _status_label = lv_label_create(lv_screen_active());
    lv_obj_set_pos(_status_label, 4, 132);
    lv_obj_set_width(_status_label, 312);
    lv_obj_set_style_text_color(_status_label, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_long_mode(_status_label, LV_LABEL_LONG_MODE_CLIP);

    _queue_label = lv_label_create(lv_screen_active());
    lv_obj_set_pos(_queue_label, 4, 151);
    lv_obj_set_width(_queue_label, 312);
    lv_obj_set_style_text_color(_queue_label, lv_color_hex(0xC6D6D4), 0);
    lv_label_set_long_mode(_queue_label, LV_LABEL_LONG_MODE_CLIP);

    add_button("SETUP", 4, 185, 100, 38, ButtonAction::Setup, false);
    add_button("ARM", 110, 185, 100, 38, ButtonAction::Arm, false);
    add_button("SCAN NOW", 216, 185, 100, 38, ButtonAction::ScanNow, false);

    const std::array<const char*, 12> labels{
        "YAW -", "PITCH +", "PITCH -", "YAW +",
        "SET L", "SET C", "SET R", "TO CENTER",
        "TEST", "CONFIRM", "SENS", "BACK",
    };
    const std::array<ButtonAction, 12> actions{
        ButtonAction::YawLeft, ButtonAction::PitchUp, ButtonAction::PitchDown, ButtonAction::YawRight,
        ButtonAction::MarkLeft, ButtonAction::MarkCenter, ButtonAction::MarkRight, ButtonAction::MoveWatch,
        ButtonAction::TestSweep, ButtonAction::ConfirmClearance, ButtonAction::Sensitivity, ButtonAction::Back,
    };
    for (std::size_t i = 0; i < labels.size(); ++i) {
        const int row = static_cast<int>(i / 4);
        const int column = static_cast<int>(i % 4);
        add_button(labels[i], column * 79 + 3, 104 + row * 38, 76, 34, actions[i], true);
    }
}

void AppSurveillance::add_button(const char* label, int x, int y, int width, int height,
                                 ButtonAction action, bool setup_button)
{
    lv_obj_t* button = lv_button_create(lv_screen_active());
    lv_obj_set_size(button, width, height);
    lv_obj_set_pos(button, x, y);
    const auto main_default = static_cast<lv_style_selector_t>(LV_PART_MAIN) |
                              static_cast<lv_style_selector_t>(LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x17685F), main_default);
    lv_obj_set_style_radius(button, 5, main_default);
    lv_obj_t* text = lv_label_create(button);
    lv_label_set_text(text, label);
    lv_obj_set_style_text_font(text, &lv_font_montserrat_14, 0);
    lv_obj_center(text);
    auto context = std::make_unique<ButtonContext>();
    context->app = this;
    context->action = action;
    lv_obj_add_event_cb(button, button_event, LV_EVENT_CLICKED, context.get());
    _button_contexts.push_back(std::move(context));
    if (setup_button) {
        for (lv_obj_t*& slot : _setup_buttons) {
            if (!slot) { slot = button; break; }
        }
        lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
    } else {
        for (lv_obj_t*& slot : _main_buttons) {
            if (!slot) { slot = button; break; }
        }
    }
}

void AppSurveillance::button_event(lv_event_t* event)
{
    auto* context = static_cast<ButtonContext*>(lv_event_get_user_data(event));
    if (context && context->app) {
        // Servo and state-machine actions run in onRunning(), not LVGL's event callback.
        context->app->_pending_action.store(context->action);
    }
}

void AppSurveillance::handle_button(ButtonAction action)
{
    switch (action) {
        case ButtonAction::None:
            break;
        case ButtonAction::Setup:
            _setup_page = true;
            break;
        case ButtonAction::Back:
            _setup_page = false;
            break;
        case ButtonAction::Sensitivity: {
            const unsigned next = _sensitivity.load() % 3 + 1;
            _sensitivity.store(next);
            save_settings();
            const char* level = next == 1 ? "low · 6%" : next == 2 ? "medium · 3%" : "high · 1%";
            char message[72];
            std::snprintf(message, sizeof(message), "Motion sensitivity %s", level);
            update_status(message);
            break;
        }
        case ButtonAction::Arm:
            if (_armed.load()) {
                _armed.store(false);
                _schedule.disarm();
                if (_scan_active) fail_scan("disarmed by user");
                _suspend_detection.store(true);
                _reset_baseline.store(true);
                update_status("Disarmed");
                break;
            }
            if (!SURVEILLANCE_PROFILE_CONFIGURED) {
                update_status("Pair receiver over USB before arming");
                break;
            }
            if (!_sweep_verified) {
                update_status("Setup required · complete supervised sweep first");
                _setup_page = true;
                break;
            }
            {
                surveillance::ServoFeedback feedback;
                if (!read_servo_feedback(feedback) ||
                    surveillance::position_drifted(feedback, _config.center_yaw, _config.pitch)) {
                    update_status("Move to watch position before arming");
                    break;
                }
                _watch_yaw = _config.center_yaw;
                _watch_pitch = _config.pitch;
                _armed.store(true);
                _schedule.arm(monotonic_ms());
                _suspend_detection.store(false);
                _reset_baseline.store(true);
                update_status("Armed · initial scan queued");
            }
            break;
        case ButtonAction::ScanNow:
            start_scan(false);
            break;
        case ButtonAction::YawLeft:
        case ButtonAction::YawRight:
        case ButtonAction::PitchUp:
        case ButtonAction::PitchDown: {
            surveillance::ServoFeedback feedback;
            if (!read_servo_feedback(feedback)) {
                update_status("Jog stopped · servo feedback invalid");
                break;
            }
            const int yaw_delta = action == ButtonAction::YawLeft ? -50 :
                                  action == ButtonAction::YawRight ? 50 : 0;
            const int pitch_delta = action == ButtonAction::PitchUp ? 50 :
                                    action == ButtonAction::PitchDown ? -50 : 0;
            _jog_yaw_target = std::clamp(feedback.yaw + yaw_delta, surveillance::kYawMin, surveillance::kYawMax);
            _jog_pitch_target = std::clamp(feedback.pitch + pitch_delta,
                                           surveillance::kSetupPitchMin, surveillance::kSetupPitchMax);
            if (yaw_delta != 0 || pitch_delta != 0) {
                _sweep_verified = false;
                _test_sweep_completed = false;
                GetStackChan().motion().moveWithSpeed(_jog_yaw_target, _jog_pitch_target, kServoSpeed);
                char message[80];
                std::snprintf(message, sizeof(message), "Jogging · yaw %d · tilt %d", _jog_yaw_target / 10,
                              _jog_pitch_target / 10);
                update_status(message);
            }
            break;
        }
        case ButtonAction::MarkLeft:
        case ButtonAction::MarkCenter:
        case ButtonAction::MarkRight: {
            surveillance::ServoFeedback feedback;
            if (!read_servo_feedback(feedback) || feedback.moving) {
                update_status("Cannot teach view · stop and verify servo first");
                break;
            }
            if (action == ButtonAction::MarkLeft) _config.left_yaw = feedback.yaw;
            if (action == ButtonAction::MarkCenter) {
                _config.center_yaw = feedback.yaw;
                _config.pitch = feedback.pitch;
            }
            if (action == ButtonAction::MarkRight) _config.right_yaw = feedback.yaw;
            _sweep_verified = false;
            _test_sweep_completed = false;
            if (surveillance::ScanController::valid_config(_config)) {
                save_settings();
                update_status("View saved · supervise a test sweep before arming");
            } else {
                update_status("Views must be ordered left < center < right · adjust and save");
            }
            break;
        }
        case ButtonAction::MoveWatch: {
            _sweep_verified = false;
            _test_sweep_completed = false;
            GetStackChan().motion().moveWithSpeed(_config.center_yaw, _config.pitch, kServoSpeed);
            update_status("Moving to watch position at speed 200");
            break;
        }
        case ButtonAction::TestSweep:
            if (!surveillance::ScanController::valid_config(_config)) {
                update_status("Teach ordered left, center and right views first");
            } else {
                update_status("Supervise sweep · check views, clearance, cable and stability");
                start_scan(true);
            }
            break;
        case ButtonAction::ConfirmClearance:
            if (!_test_sweep_completed) {
                update_status("Run the supervised test sweep before confirming");
            } else {
                _sweep_verified = true;
                save_settings();
                update_status("Setup confirmed · Arm when positioned at watch view");
            }
            break;
    }
}

void AppSurveillance::start_scan(bool supervised_test)
{
    if (_home_closing || _scan_active) return;
    if (_burst_active.load()) {
        update_status("Scan waiting for current motion burst to finish");
        return;
    }
    if (!surveillance::ScanController::valid_config(_config)) {
        update_status("Invalid scan views or tilt outside 10–80 degrees");
        return;
    }
    std::lock_guard<std::mutex> activity_lock(_activity_mutex);
    if (_burst_active.load()) {
        update_status("Scan waiting for current motion burst to finish");
        return;
    }
    surveillance::ServoFeedback feedback;
    if (!read_servo_feedback(feedback) || feedback.moving ||
        absolute(feedback.pitch - _config.pitch) > 20) {
        update_status("Scan paused · verify tilt and servo feedback");
        return;
    }
    if (!_scan.start(monotonic_ms(), _config)) {
        update_status("Scan configuration rejected");
        return;
    }
    make_id(_scan_group_id);
    {
        std::lock_guard<std::mutex> status_lock(_status_mutex);
        std::snprintf(_last_scan_group_id, sizeof(_last_scan_group_id), "%s", _scan_group_id);
        std::snprintf(_scan_record_state, sizeof(_scan_record_state), "scanning");
        _scan_record_message[0] = '\0';
    }
    _scan_active = true;
    _scan_is_test = supervised_test;
    _scan_capture_queued = false;
    _suspend_detection.store(true);
    handle_scan_action(_scan.initial_action());
    ESP_LOGI(TAG, "scan started supervised=%d group=%s", supervised_test, _scan_group_id);
}

void AppSurveillance::handle_scan_action(const surveillance::ScanAction& action)
{
    switch (action.kind) {
        case surveillance::ScanActionKind::None:
            break;
        case surveillance::ScanActionKind::Move:
            GetStackChan().motion().moveWithSpeed(action.yaw, action.pitch, kServoSpeed);
            ESP_LOGI(TAG, "scan move view=%u yaw=%d pitch=%d speed=%d", action.view_index,
                     action.yaw, action.pitch, kServoSpeed);
            break;
        case surveillance::ScanActionKind::Capture: {
            if (_scan_capture_queued || !_scan_requests) break;
            CaptureRequest request{};
            std::snprintf(request.group_id, sizeof(request.group_id), "%s", _scan_group_id);
            request.view_index = static_cast<std::uint8_t>(action.view_index);
            if (xQueueSend(_scan_requests, &request, 0) == pdPASS) {
                _scan_capture_queued = true;
                ESP_LOGI(TAG, "scan position settled; capture view=%u", action.view_index);
            } else {
                handle_scan_action(_scan.capture_completed(monotonic_ms(), false));
            }
            break;
        }
        case surveillance::ScanActionKind::Complete:
            finish_scan();
            break;
        case surveillance::ScanActionKind::Failed:
            fail_scan(_scan.failure());
            break;
    }
}

void AppSurveillance::finish_scan()
{
    if (_schedule.armed()) _schedule.completed(monotonic_ms());
    {
        std::lock_guard<std::mutex> status_lock(_status_mutex);
        std::snprintf(_scan_record_state, sizeof(_scan_record_state), "complete");
        std::snprintf(_scan_record_message, sizeof(_scan_record_message), "all three views captured; returned to center");
    }
    const bool was_test = _scan_is_test;
    _scan_active = false;
    _scan_capture_queued = false;
    _suspend_detection.store(!_armed.load());
    _reset_baseline.store(true);
    if (was_test) {
        _test_sweep_completed = true;
        update_status("Sweep complete · inspect camera, cable and released-motor stability");
    } else {
        update_status(_armed.load() ? "Scan complete · watching center view" : "Scan complete");
    }
    ESP_LOGI(TAG, "scan complete test=%d", was_test);
}

void AppSurveillance::fail_scan(const char* reason)
{
    const bool was_scanning = _scan_active;
    if (was_scanning) _scan.cancel(reason);
    if (was_scanning) {
        std::lock_guard<std::mutex> status_lock(_status_mutex);
        std::snprintf(_scan_record_state, sizeof(_scan_record_state), "failed");
        std::snprintf(_scan_record_message, sizeof(_scan_record_message), "%s", reason ? reason : "scan failed");
    }
    _scan_active = false;
    _scan_capture_queued = false;
    _schedule.disarm();
    _armed.store(false);
    _suspend_detection.store(true);
    _reset_baseline.store(true);
    GetStackChan().motion().cancelAndRelease();
    char message[112];
    std::snprintf(message, sizeof(message), "PAUSED · %s", reason ? reason : "servo fault");
    update_status(message);
    ESP_LOGE(TAG, "scan cancelled and torque released: %s", reason ? reason : "servo fault");
}

void AppSurveillance::begin_home_close()
{
    _home_closing = true;
    _home_started_us = static_cast<std::uint64_t>(esp_timer_get_time());
    _armed.store(false);
    _schedule.disarm();
    _suspend_detection.store(true);
    _stop_capture.store(true);
    _stop_upload.store(true);  // uploader finishes the bounded RAM queue before resources are released
    if (_scan_active) {
        _scan.cancel("Home requested");
        std::lock_guard<std::mutex> status_lock(_status_mutex);
        std::snprintf(_scan_record_state, sizeof(_scan_record_state), "failed");
        std::snprintf(_scan_record_message, sizeof(_scan_record_message), "cancelled by Home");
        _heartbeat_requested.store(true);
    }
    _scan_active = false;
    GetStackChan().motion().cancelAndRelease();
    update_status("Home · stopping capture and draining queued images");
}

void AppSurveillance::load_settings()
{
    Settings settings(kSettingsNamespace, false);
    _config.left_yaw = settings.GetInt("left_yaw", -450);
    _config.center_yaw = settings.GetInt("center_yaw", 0);
    _config.right_yaw = settings.GetInt("right_yaw", 450);
    _config.pitch = settings.GetInt("pitch", 450);
    _sensitivity.store(static_cast<unsigned>(std::clamp<std::int32_t>(settings.GetInt("sensitivity", 2), 1, 3)));
    _sweep_verified = settings.GetInt("sweep_verified", 0) == 1;
    _lost_images.store(static_cast<std::uint32_t>(std::max<std::int32_t>(0, settings.GetInt("lost_images", 0))));
    if (!surveillance::ScanController::valid_config(_config)) {
        _config = {};
        _sweep_verified = false;
    }
    _watch_yaw = _config.center_yaw;
    _watch_pitch = _config.pitch;
}

void AppSurveillance::save_settings()
{
    Settings settings(kSettingsNamespace, true);
    settings.SetInt("left_yaw", _config.left_yaw);
    settings.SetInt("center_yaw", _config.center_yaw);
    settings.SetInt("right_yaw", _config.right_yaw);
    settings.SetInt("pitch", _config.pitch);
    settings.SetInt("sensitivity", static_cast<int>(_sensitivity.load()));
    settings.SetInt("sweep_verified", _sweep_verified ? 1 : 0);
    settings.SetInt("lost_images", static_cast<int>(_lost_images.load()));
}

bool AppSurveillance::read_servo_feedback(surveillance::ServoFeedback& feedback)
{
    int yaw = 0;
    int pitch = 0;
    bool yaw_moving = false;
    bool pitch_moving = false;
    auto& motion = GetStackChan().motion();
    const bool yaw_valid = motion.yawServo().readFeedback(yaw, yaw_moving);
    const bool pitch_valid = motion.pitchServo().readFeedback(pitch, pitch_moving);
    feedback = {yaw, pitch, yaw_moving || pitch_moving, yaw_valid && pitch_valid};
    return feedback.valid;
}

void AppSurveillance::update_status(const char* error)
{
    if (!error) return;
    std::lock_guard<std::mutex> lock(_status_mutex);
    std::snprintf(_error, sizeof(_error), "%s", error);
}

void AppSurveillance::update_ui()
{
    PreviewFrame preview{};
    const bool have_preview = _preview_frames && xQueueReceive(_preview_frames, &preview, 0) == pdTRUE;
    const auto now = monotonic_ms();
    if (!have_preview && now - _last_ui_update_ms < 250) return;
    _last_ui_update_ms = now;

    LvglLockGuard lock;
    if (have_preview && _preview_pixels) {
        auto* rgb565 = reinterpret_cast<std::uint16_t*>(_preview_pixels);
        for (std::size_t i = 0; i < preview.grayscale.size(); ++i) {
            const std::uint16_t gray = preview.grayscale[i] >> 3;
            const std::uint16_t green = preview.grayscale[i] >> 2;
            rgb565[i] = static_cast<std::uint16_t>((gray << 11) | (green << 5) | gray);
        }
    }

    char error[sizeof(_error)];
    char saved[sizeof(_last_saved_capture_id)];
    {
        std::lock_guard<std::mutex> lock(_status_mutex);
        std::snprintf(error, sizeof(error), "%s", _error);
        std::snprintf(saved, sizeof(saved), "%s", _last_saved_capture_id);
    }
    const bool receiver_live = _receiver_connected.load() && now - _last_heartbeat_ms.load() <= 15'000;
    const bool configured = SURVEILLANCE_PROFILE_CONFIGURED;
    const char* state = _home_closing ? "CLOSING" : _scan_active ? "SCANNING" : _armed.load() ? "ARMED" : "DISARMED";
    char status[160];
    if (!configured) {
        std::snprintf(status, sizeof(status), "%s · receiver profile missing", state);
    } else {
        std::snprintf(status, sizeof(status), "%s · receiver %s · saved %.8s", state,
                      receiver_live ? "online" : "offline", saved[0] ? saved : "none");
    }
    char queue[224];
    std::snprintf(queue, sizeof(queue), "RAM queue %u images / %u bytes · lost %u · status: %s",
                  static_cast<unsigned>(_pending_count.load()), static_cast<unsigned>(_pending_bytes.load()),
                  static_cast<unsigned>(_lost_images.load()), error[0] ? error : "ready");

    if (_preview) {
        lv_image_set_scale(_preview, _setup_page ? 256 : 512);
        lv_obj_set_pos(_preview, 120, _setup_page ? 20 : 34);
    }
    if (_status_label) lv_obj_set_pos(_status_label, 4, _setup_page ? 190 : 132);
    if (_queue_label) lv_obj_set_pos(_queue_label, 4, _setup_page ? 209 : 151);
    if (_status_label) lv_label_set_text(_status_label, status);
    if (_queue_label) lv_label_set_text(_queue_label, queue);
    for (lv_obj_t* button : _main_buttons) {
        if (!button) continue;
        if (_setup_page) lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(button, LV_OBJ_FLAG_HIDDEN);
    }
    for (std::size_t i = 0; i < _setup_buttons.size(); ++i) {
        lv_obj_t* button = _setup_buttons[i];
        if (!button) continue;
        if (_setup_page) {
            const int row = static_cast<int>(i / 4);
            const int column = static_cast<int>(i % 4);
            lv_obj_set_pos(button, column * 79 + 3, 80 + row * 37);
        }
        if (_setup_page) lv_obj_remove_flag(button, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
    }
    if (_setup_buttons[10]) {
        const char* sensitivity_label = _sensitivity.load() == 1 ? "SENS LOW" :
                                        _sensitivity.load() == 3 ? "SENS HIGH" : "SENS MED";
        lv_label_set_text(lv_obj_get_child(_setup_buttons[10], 0), sensitivity_label);
    }
}

void AppSurveillance::capture_worker(void* context)
{
    auto* app = static_cast<AppSurveillance*>(context);
    app->run_capture_worker();
    const auto done = app->_capture_done;
    xSemaphoreGive(done);
    vTaskDelete(nullptr);
}

void AppSurveillance::run_capture_worker()
{
    auto* camera = hal_bridge::board_get_camera();
    std::array<std::uint8_t, surveillance::kMotionPixels> grayscale{};
    std::array<std::uint8_t, 33> motion_group{};
    std::uint64_t next_burst_ms = 0;
    unsigned burst_sequence = 0;
    TickType_t last_wake = xTaskGetTickCount();
    while (!_stop_capture.load()) {
        if (!camera) camera = hal_bridge::board_get_camera();
        if (!camera || !camera->StreamCaptures(true)) {
            update_status("Camera capture failed");
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kCaptureIntervalMs));
            continue;
        }
        const auto* frame = camera->GetFrameData();
        const auto frame_size = camera->GetFrameSize();
        const int width = camera->GetFrameWidth();
        const int height = camera->GetFrameHeight();
        const int format = camera->GetFrameFormat();
        if (!to_gray_80x60(frame, frame_size, width, height, format, grayscale)) {
            update_status("Unsupported 320x240 camera format");
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kCaptureIntervalMs));
            continue;
        }

        if (_preview_frames) {
            PreviewFrame preview{};
            preview.grayscale = grayscale;
            xQueueOverwrite(_preview_frames, &preview);
        }
        if (_reset_baseline.exchange(false)) _detector.reset();

        CaptureRequest request{};
        const bool has_scan_request = _scan_requests && xQueueReceive(_scan_requests, &request, 0) == pdTRUE;
        if (has_scan_request) {
            static const char* views[] = {"left", "center", "right"};
            const unsigned view = std::min<unsigned>(request.view_index, 2);
            const bool queued = encode_and_enqueue(frame, frame_size, width, height, format,
                                                  request.group_id, "scan", views[view], view, monotonic_ms());
            publish_scan_capture(request, queued);
        } else if (_burst_active.load()) {
            const auto now = monotonic_ms();
            if (now >= next_burst_ms && burst_sequence < 3) {
                const bool queued = encode_and_enqueue(frame, frame_size, width, height, format,
                                                      reinterpret_cast<const char*>(motion_group.data()),
                                                      "motion", "fixed", burst_sequence, now);
                if (!queued) update_status("Motion image could not enter RAM queue");
                ++burst_sequence;
                next_burst_ms += 1000;
                if (burst_sequence >= 3) {
                    _burst_active.store(false);
                    ESP_LOGI(TAG, "motion burst capture complete");
                }
            }
        } else {
            surveillance::MotionResult result = surveillance::MotionResult::Suspended;
            {
                std::lock_guard<std::mutex> activity_lock(_activity_mutex);
                _detector.set_sensitivity_level(_sensitivity.load());
                result = _detector.process(grayscale.data(), grayscale.size(), monotonic_ms(),
                                           _armed.load() && !_suspend_detection.load());
                if (result == surveillance::MotionResult::Detected) {
                    make_id(reinterpret_cast<char*>(motion_group.data()));
                    burst_sequence = 0;
                    next_burst_ms = monotonic_ms();
                    _burst_active.store(true);
                }
            }
            if (result == surveillance::MotionResult::Detected) {
                const auto now = monotonic_ms();
                const bool queued = encode_and_enqueue(frame, frame_size, width, height, format,
                                                      reinterpret_cast<const char*>(motion_group.data()),
                                                      "motion", "fixed", burst_sequence, now);
                if (!queued) update_status("Motion image could not enter RAM queue");
                ++burst_sequence;
                next_burst_ms = now + 1000;
            }
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kCaptureIntervalMs));
    }
}

bool AppSurveillance::encode_and_enqueue(const std::uint8_t* frame, std::size_t frame_size,
                                        int width, int height, int format,
                                        const char* group_id, const char* kind,
                                        const char* view, unsigned sequence, std::uint64_t captured_ms)
{
    auto* image = new (std::nothrow) UploadImage{};
    if (!image) {
        ++_lost_images;
        update_status("Not enough memory for image metadata");
        return false;
    }
    if (!image_to_jpeg(const_cast<std::uint8_t*>(frame), frame_size, width, height,
                       static_cast<v4l2_pix_fmt_t>(format), 75, &image->jpeg, &image->size) ||
        !image->jpeg || !image->size) {
        release_image(image);
        ++_lost_images;
        update_status("JPEG encoding failed");
        return false;
    }
    image->monotonic_ms = captured_ms;
    image->sequence = static_cast<std::uint8_t>(std::min(sequence, 255u));
    make_short_id(image->capture_id);
    std::snprintf(image->group_id, sizeof(image->group_id), "%s", group_id);
    std::snprintf(image->kind, sizeof(image->kind), "%s", kind);
    std::snprintf(image->view, sizeof(image->view), "%s", view);
    if (!enqueue_image(image)) {
        release_image(image);
        ++_lost_images;
        return false;
    }
    return true;
}

bool AppSurveillance::take_queue_lock(TickType_t wait)
{
    return _queue_guard && xSemaphoreTake(_queue_guard, wait) == pdTRUE;
}

bool AppSurveillance::enqueue_image(UploadImage* image)
{
    if (!image || !_uploads || image->size > kMaxPendingBytes || !take_queue_lock()) return false;
    while (surveillance::queue_needs_oldest_drop(_pending_count.load(), _pending_bytes.load(), image->size)) {
        UploadImage* oldest = nullptr;
        if (xQueueReceive(_uploads, &oldest, 0) != pdPASS || !oldest) {
            xSemaphoreGive(_queue_guard);
            return false;
        }
        _pending_count.fetch_sub(1);
        _pending_bytes.fetch_sub(oldest->size);
        release_image(oldest);
        ++_lost_images;
    }
    const bool queued = xQueueSend(_uploads, &image, 0) == pdPASS;
    if (queued) {
        _pending_count.fetch_add(1);
        _pending_bytes.fetch_add(image->size);
    }
    xSemaphoreGive(_queue_guard);
    return queued;
}

void AppSurveillance::release_image(UploadImage* image)
{
    if (!image) return;
    std::free(image->jpeg);
    image->jpeg = nullptr;
    delete image;
}

void AppSurveillance::publish_scan_capture(const CaptureRequest& request, bool queued)
{
    if (!_scan_results) return;
    ScanCaptureResult result{request.view_index, queued};
    xQueueOverwrite(_scan_results, &result);
}

void AppSurveillance::upload_worker(void* context)
{
    auto* app = static_cast<AppSurveillance*>(context);
    app->run_upload_worker();
    const auto done = app->_upload_done;
    xSemaphoreGive(done);
    vTaskDelete(nullptr);
}

void AppSurveillance::run_upload_worker()
{
#if SURVEILLANCE_PROFILE_CONFIGURED
    UploadImage* active = nullptr;
    std::uint64_t next_heartbeat = 0;
    std::uint64_t next_attempt = 0;
    std::uint32_t retry_ms = 1000;
    while (true) {
        if (!active && _uploads && take_queue_lock()) {
            xQueueReceive(_uploads, &active, 0);
            xSemaphoreGive(_queue_guard);
        }
        const auto now = monotonic_ms();
        const bool final_scan_heartbeat = _heartbeat_requested.exchange(false);
        const bool send_periodic_heartbeat = !_stop_upload.load() && now >= next_heartbeat;
        const bool send_final_scan_heartbeat = final_scan_heartbeat && _receiver_connected.load();
        if (send_periodic_heartbeat || send_final_scan_heartbeat) {
            if (ensure_network() && send_heartbeat()) {
                _last_heartbeat_ms.store(now);
                _receiver_connected.store(true);
            } else {
                _receiver_connected.store(false);
                update_status("Receiver heartbeat failed; retrying");
            }
            next_heartbeat = now + kHeartbeatIntervalMs;
        }
        if (active && now >= next_attempt) {
            if (ensure_network() && upload_image(*active)) {
                {
                    std::lock_guard<std::mutex> lock(_status_mutex);
                    std::snprintf(_last_saved_capture_id, sizeof(_last_saved_capture_id), "%s", active->capture_id);
                }
                while (!take_queue_lock()) vTaskDelay(pdMS_TO_TICKS(10));
                _pending_count.fetch_sub(1);
                _pending_bytes.fetch_sub(active->size);
                xSemaphoreGive(_queue_guard);
                release_image(active);
                active = nullptr;
                retry_ms = 1000;
                next_attempt = 0;
            } else {
                _receiver_connected.store(false);
                if (_stop_upload.load()) {
                    std::array<UploadImage*, kMaxPendingImages + 1> discarded{};
                    std::size_t discarded_count = 0;
                    while (!take_queue_lock()) vTaskDelay(pdMS_TO_TICKS(10));
                    if (active) {
                        discarded[discarded_count++] = active;
                        _pending_count.fetch_sub(1);
                        _pending_bytes.fetch_sub(active->size);
                        active = nullptr;
                    }
                    UploadImage* queued = nullptr;
                    while (discarded_count < discarded.size() &&
                           xQueueReceive(_uploads, &queued, 0) == pdPASS) {
                        if (!queued) continue;
                        discarded[discarded_count++] = queued;
                        _pending_count.fetch_sub(1);
                        _pending_bytes.fetch_sub(queued->size);
                    }
                    xSemaphoreGive(_queue_guard);
                    for (std::size_t i = 0; i < discarded_count; ++i) release_image(discarded[i]);
                    _lost_images.fetch_add(static_cast<std::uint32_t>(discarded_count));
                    update_status("Home · remaining RAM images were not confirmed and were discarded");
                    break;
                }
                update_status("Upload failed · image retained in RAM");
                next_attempt = now + retry_ms;
                retry_ms = std::min<std::uint32_t>(retry_ms * 2, 30'000);
            }
        }
        if (_stop_upload.load() && !active && _pending_count.load() == 0) break;
        if (!active && _pending_count.load() == 0 && _stop_upload.load()) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (active) {
        // Reached only after an explicit forced shutdown path.
        release_image(active);
        ++_lost_images;
    }
#else
    while (!_stop_upload.load()) {
        UploadImage* image = nullptr;
        if (_uploads && take_queue_lock()) {
            const bool received = xQueueReceive(_uploads, &image, 0) == pdPASS;
            if (received && image) {
                _pending_count.fetch_sub(1);
                _pending_bytes.fetch_sub(image->size);
            }
            xSemaphoreGive(_queue_guard);
        }
        if (image) {
            release_image(image);
            ++_lost_images;
            update_status("Image discarded · provision receiver profile");
        } else {
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
    std::array<UploadImage*, kMaxPendingImages> discarded{};
    std::size_t discarded_count = 0;
    if (take_queue_lock()) {
        UploadImage* image = nullptr;
        while (discarded_count < discarded.size() && xQueueReceive(_uploads, &image, 0) == pdPASS) {
            if (!image) continue;
            discarded[discarded_count++] = image;
            _pending_count.fetch_sub(1);
            _pending_bytes.fetch_sub(image->size);
        }
        xSemaphoreGive(_queue_guard);
    }
    for (std::size_t i = 0; i < discarded_count; ++i) release_image(discarded[i]);
    if (discarded_count) {
        _lost_images.fetch_add(static_cast<std::uint32_t>(discarded_count));
        update_status("Home · unpaired RAM images were discarded");
    }
#endif
}

bool AppSurveillance::ensure_network()
{
    const auto deadline = monotonic_ms() + 15'000;
    return GetHAL().startNetworkCancellable(
        [deadline]() { return monotonic_ms() >= deadline; },
        [this](std::string_view message) {
            char text[112];
            std::snprintf(text, sizeof(text), "%.*s", static_cast<int>(std::min<std::size_t>(message.size(), 100)), message.data());
            update_status(text);
        });
}

bool AppSurveillance::send_http_request(const char* method, const char* path, const char* content_type,
                                       const void* payload, std::size_t payload_size,
                                       int& status_code, char* response_body, std::size_t response_capacity,
                                       const UploadImage* image)
{
    char ip[16];
    if (!resolve_mdns_ipv4(kSurveillanceReceiverHost, ip)) return false;
    char url[256];
    std::snprintf(url, sizeof(url), "https://%s:%d%s", ip, kSurveillanceReceiverPort, path);

    HttpResponseBuffer response{};
    esp_http_client_config_t config{};
    config.url = url;
    config.timeout_ms = kHttpTimeoutMs;
    config.cert_pem = kSurveillanceCaPem;
    config.disable_auto_redirect = true;
    config.common_name = kSurveillanceReceiverHost;
    config.event_handler = http_event;
    config.user_data = &response;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;
    esp_http_client_set_method(client, std::strcmp(method, "PUT") == 0 ? HTTP_METHOD_PUT : HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Host", kSurveillanceReceiverHost);
    esp_http_client_set_header(client, "X-StackChan-Token", kSurveillancePairingToken);
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_header(client, "Connection", "close");
    if (image) {
        char sequence[8];
        char capture_mono[24];
        char capture_epoch[24];
        std::snprintf(sequence, sizeof(sequence), "%u", image->sequence);
        std::snprintf(capture_mono, sizeof(capture_mono), "%llu",
                      static_cast<unsigned long long>(image->monotonic_ms));
        const auto anchor_mono = _clock_anchor_monotonic_ms.load();
        const auto anchor_utc = _clock_anchor_receiver_ms.load();
        std::uint64_t capture_utc = 0;
        if (anchor_utc && image->monotonic_ms >= anchor_mono)
            capture_utc = anchor_utc + (image->monotonic_ms - anchor_mono);
        else if (anchor_utc && anchor_mono >= image->monotonic_ms &&
                 anchor_utc >= anchor_mono - image->monotonic_ms)
            capture_utc = anchor_utc - (anchor_mono - image->monotonic_ms);
        std::snprintf(capture_epoch, sizeof(capture_epoch), "%llu",
                      static_cast<unsigned long long>(capture_utc));
        esp_http_client_set_header(client, "X-Capture-Group", image->group_id);
        esp_http_client_set_header(client, "X-Capture-Kind", image->kind);
        esp_http_client_set_header(client, "X-Capture-View", image->view);
        esp_http_client_set_header(client, "X-Capture-Sequence", sequence);
        esp_http_client_set_header(client, "X-Device-Boot", _boot_id);
        esp_http_client_set_header(client, "X-Capture-Monotonic-Ms", capture_mono);
        esp_http_client_set_header(client, "X-Capture-Receiver-Time-Ms", capture_epoch);
    }
    esp_http_client_set_post_field(client, static_cast<const char*>(payload), static_cast<int>(payload_size));
    const esp_err_t result = esp_http_client_perform(client);
    status_code = esp_http_client_get_status_code(client);
    const bool successful = result == ESP_OK && status_code >= 200 && status_code < 300;
    if (response_body && response_capacity > 0) {
        std::snprintf(response_body, response_capacity, "%s", response.data);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return successful;
}

bool AppSurveillance::send_heartbeat()
{
    char status_message[sizeof(_error)];
    char scan_group[sizeof(_last_scan_group_id)];
    char scan_state[sizeof(_scan_record_state)];
    char scan_message[sizeof(_scan_record_message)];
    {
        std::lock_guard<std::mutex> lock(_status_mutex);
        std::snprintf(status_message, sizeof(status_message), "%s", _error);
        std::snprintf(scan_group, sizeof(scan_group), "%s", _last_scan_group_id);
        std::snprintf(scan_state, sizeof(scan_state), "%s", _scan_record_state);
        std::snprintf(scan_message, sizeof(scan_message), "%s", _scan_record_message);
    }
    char safe_status[sizeof(status_message) * 2]{};
    char safe_scan_message[sizeof(scan_message) * 2]{};
    escape_json(status_message, safe_status, sizeof(safe_status));
    escape_json(scan_message, safe_scan_message, sizeof(safe_scan_message));
    char payload[896];
    const char* state = _scan_active ? "scanning" : _armed.load() ? "armed" : "disarmed";
    const int size = std::snprintf(payload, sizeof(payload),
        "{\"boot_id\":\"%s\",\"monotonic_ms\":%llu,\"state\":\"%s\",\"queued_images\":%u,\"queued_bytes\":%u,\"lost_images\":%u,\"status\":\"%s\",\"scan_group_id\":\"%s\",\"scan_status\":\"%s\",\"scan_message\":\"%s\"}",
        _boot_id, static_cast<unsigned long long>(monotonic_ms()), state,
        static_cast<unsigned>(_pending_count.load()), static_cast<unsigned>(_pending_bytes.load()),
        static_cast<unsigned>(_lost_images.load()), safe_status, scan_group, scan_state, safe_scan_message);
    int status = 0;
    char response[320]{};
    if (size <= 0 || static_cast<std::size_t>(size) >= sizeof(payload) ||
        !send_http_request("POST", "/v1/heartbeat", "application/json", payload,
                           static_cast<std::size_t>(size), status, response, sizeof(response))) return false;
    const char* field = std::strstr(response, "\"receiver_time_ms\":");
    if (field) {
        const auto receiver_ms = std::strtoull(field + std::strlen("\"receiver_time_ms\":"), nullptr, 10);
        _clock_anchor_receiver_ms.store(receiver_ms);
        _clock_anchor_monotonic_ms.store(monotonic_ms());
    }
    const char* storage = std::strstr(response, "\"storage_ok\"");
    const char* storage_value = storage ? std::strchr(storage, ':') : nullptr;
    if (storage_value) {
        ++storage_value;
        while (*storage_value == ' ' || *storage_value == '\t') ++storage_value;
        if (std::strncmp(storage_value, "false", 5) == 0) {
            update_status("Receiver disk storage error · images are not confirmed saved");
        } else if (std::strncmp(storage_value, "true", 4) == 0) {
            std::lock_guard<std::mutex> lock(_status_mutex);
            if (std::strcmp(_error, "Receiver disk storage error · images are not confirmed saved") == 0) {
                _error[0] = '\0';
            }
        }
    }
    return true;
}

bool AppSurveillance::upload_image(const UploadImage& image)
{
    char path[96];
    std::snprintf(path, sizeof(path), "/v1/frames/%s", image.capture_id);
    int status = 0;
    return send_http_request("PUT", path, "image/jpeg", image.jpeg, image.size,
                             status, nullptr, 0, &image) && (status == 200 || status == 201);
}

bool AppSurveillance::poll_worker_completion()
{
    bool done = true;
    if (_capture_started) {
        if (_capture_done && xSemaphoreTake(_capture_done, 0) == pdTRUE) {
            _capture_started = false;
            _capture_task = nullptr;
        } else done = false;
    }
    if (_upload_started) {
        if (_upload_done && xSemaphoreTake(_upload_done, 0) == pdTRUE) {
            _upload_started = false;
            _upload_task = nullptr;
        } else done = false;
    }
    return done;
}

void AppSurveillance::stop_workers()
{
    _stop_capture.store(true);
    _stop_upload.store(true);
    if (_capture_started && _capture_done) {
        xSemaphoreTake(_capture_done, portMAX_DELAY);
        _capture_started = false;
    }
    if (_upload_started && _upload_done) {
        xSemaphoreTake(_upload_done, portMAX_DELAY);
        _upload_started = false;
    }
    if (_uploads) {
        UploadImage* image = nullptr;
        while (xQueueReceive(_uploads, &image, 0) == pdPASS) release_image(image);
        vQueueDelete(_uploads);
        _uploads = nullptr;
    }
    if (_preview_frames) { vQueueDelete(_preview_frames); _preview_frames = nullptr; }
    if (_scan_requests) { vQueueDelete(_scan_requests); _scan_requests = nullptr; }
    if (_scan_results) { vQueueDelete(_scan_results); _scan_results = nullptr; }
    if (_queue_guard) { vSemaphoreDelete(_queue_guard); _queue_guard = nullptr; }
    if (_capture_done) { vSemaphoreDelete(_capture_done); _capture_done = nullptr; }
    if (_upload_done) { vSemaphoreDelete(_upload_done); _upload_done = nullptr; }
    _resources_ready = false;
}

void AppSurveillance::make_id(char output[33])
{
    make_short_id(output);
}
