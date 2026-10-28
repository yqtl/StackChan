#pragma once

#include "motion_detector.hpp"
#include "scan_controller.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <lvgl.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mooncake.h>

class AppSurveillance : public mooncake::AppAbility {
public:
    AppSurveillance();
    ~AppSurveillance() override;

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    enum class ButtonAction : std::uintptr_t {
        None, Setup, Arm, ScanNow, Back, YawLeft, YawRight, PitchUp, PitchDown,
        MarkLeft, MarkCenter, MarkRight, MoveWatch, TestSweep, ConfirmClearance, Sensitivity,
    };

    struct ButtonContext {
        AppSurveillance* app = nullptr;
        ButtonAction action = ButtonAction::None;
    };

    struct PreviewFrame {
        std::array<std::uint8_t, surveillance::kMotionPixels> grayscale{};
    };

    struct CaptureRequest {
        char group_id[49]{};
        std::uint8_t view_index = 0;
    };

    struct ScanCaptureResult {
        std::uint8_t view_index = 0;
        bool saved_to_ram = false;
    };

    struct UploadImage {
        std::uint8_t* jpeg = nullptr;
        std::size_t size = 0;
        std::uint64_t monotonic_ms = 0;
        std::uint8_t sequence = 0;
        char capture_id[33]{};
        char group_id[49]{};
        char kind[8]{};
        char view[8]{};
    };

    static void capture_worker(void* context);
    static void upload_worker(void* context);
    static void button_event(lv_event_t* event);
    void run_capture_worker();
    void run_upload_worker();
    void handle_button(ButtonAction action);
    void handle_scan_action(const surveillance::ScanAction& action);
    void start_scan(bool supervised_test);
    void finish_scan();
    void fail_scan(const char* reason);
    void begin_home_close();
    void load_settings();
    void save_settings();
    void create_ui();
    void add_button(const char* label, int x, int y, int width, int height,
                    ButtonAction action, bool setup_button);
    void update_ui();
    void update_status(const char* error = nullptr);
    bool read_servo_feedback(surveillance::ServoFeedback& feedback);
    bool enqueue_image(UploadImage* image);
    bool encode_and_enqueue(const std::uint8_t* frame, std::size_t frame_size,
                            int width, int height, int format,
                            const char* group_id, const char* kind,
                            const char* view, unsigned sequence, std::uint64_t monotonic_ms);
    void release_image(UploadImage* image);
    bool send_http_request(const char* method, const char* path, const char* content_type,
                           const void* payload, std::size_t payload_size,
                           int& status_code, char* response, std::size_t response_capacity,
                           const UploadImage* image = nullptr);
    bool send_heartbeat();
    bool upload_image(const UploadImage& image);
    bool ensure_network();
    bool take_queue_lock(TickType_t wait = pdMS_TO_TICKS(100));
    void publish_scan_capture(const CaptureRequest& request, bool queued);
    void stop_workers();
    bool poll_worker_completion();
    static void make_id(char output[33]);

    surveillance::MotionDetector _detector;
    surveillance::ScanController _scan;
    surveillance::ScanSchedule _schedule;
    surveillance::ScanConfig _config{};

    std::atomic<bool> _armed{false};
    std::atomic<bool> _stop_capture{true};
    std::atomic<bool> _stop_upload{true};
    std::atomic<bool> _suspend_detection{false};
    std::atomic<bool> _burst_active{false};
    std::atomic<bool> _reset_baseline{false};
    std::atomic<bool> _receiver_connected{false};
    std::atomic<ButtonAction> _pending_action{ButtonAction::None};
    std::atomic<unsigned> _sensitivity{2};
    std::atomic<std::uint32_t> _lost_images{0};
    std::atomic<std::size_t> _pending_count{0};
    std::atomic<std::size_t> _pending_bytes{0};
    std::atomic<std::uint64_t> _clock_anchor_receiver_ms{0};
    std::atomic<std::uint64_t> _clock_anchor_monotonic_ms{0};

    QueueHandle_t _uploads = nullptr;
    QueueHandle_t _preview_frames = nullptr;
    QueueHandle_t _scan_requests = nullptr;
    QueueHandle_t _scan_results = nullptr;
    SemaphoreHandle_t _queue_guard = nullptr;
    SemaphoreHandle_t _capture_done = nullptr;
    SemaphoreHandle_t _upload_done = nullptr;
    bool _capture_started = false;
    bool _upload_started = false;
    TaskHandle_t _capture_task = nullptr;
    TaskHandle_t _upload_task = nullptr;

    lv_obj_t* _preview = nullptr;
    lv_image_dsc_t _preview_image{};
    std::uint8_t* _preview_pixels = nullptr;
    lv_obj_t* _status_label = nullptr;
    lv_obj_t* _queue_label = nullptr;
    lv_obj_t* _main_buttons[3]{};
    std::array<lv_obj_t*, 12> _setup_buttons{};
    std::vector<std::unique_ptr<ButtonContext>> _button_contexts;

    std::mutex _status_mutex;
    std::mutex _activity_mutex;
    char _error[112]{};
    char _last_saved_capture_id[33]{};
    char _boot_id[33]{};
    char _scan_group_id[49]{};
    char _last_scan_group_id[49]{};
    char _scan_record_state[12]{};
    char _scan_record_message[112]{};
    std::atomic<std::uint64_t> _last_heartbeat_ms{0};
    std::uint64_t _last_feedback_ms = 0;
    std::uint64_t _home_started_us = 0;
    std::uint64_t _last_ui_update_ms = 0;
    int _watch_yaw = 0;
    int _watch_pitch = 450;
    int _jog_yaw_target = 0;
    int _jog_pitch_target = 450;
    bool _setup_page = false;
    bool _sweep_verified = false;
    bool _test_sweep_completed = false;
    std::atomic<bool> _scan_active{false};
    bool _scan_is_test = false;
    bool _scan_capture_queued = false;
    bool _home_closing = false;
    std::atomic<bool> _home_requested{false};
    std::atomic<bool> _heartbeat_requested{false};
    bool _resources_ready = false;
};
