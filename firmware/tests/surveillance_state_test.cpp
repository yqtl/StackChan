#include "../main/apps/app_surveillance/motion_detector.hpp"
#include "../main/apps/app_surveillance/queue_budget.hpp"
#include "../main/apps/app_surveillance/scan_controller.hpp"

#include <array>
#include <cassert>
#include <cstdint>

using namespace surveillance;

static void motion_detector_cases()
{
    MotionDetector detector;
    std::array<std::uint8_t, kMotionPixels> frame{};
    frame.fill(80);
    assert(detector.process(frame.data(), frame.size(), 0) == MotionResult::Baseline);
    assert(detector.process(frame.data(), frame.size(), 500) == MotionResult::Quiet);

    // A global brightness change is compensated before pixel changes are counted.
    frame.fill(120);
    assert(detector.process(frame.data(), frame.size(), 1000) == MotionResult::Quiet);

    // Noise below the per-pixel threshold remains quiet.
    for (std::size_t i = 0; i < frame.size(); i += 3) frame[i] = 124;
    assert(detector.process(frame.data(), frame.size(), 1500) == MotionResult::Quiet);

    // More than 3% of pixels changing by at least 20/255 needs two frames.
    frame.fill(120);
    for (std::size_t i = 0; i < 250; ++i) frame[i] = 175;
    assert(detector.process(frame.data(), frame.size(), 2000) == MotionResult::Quiet);
    assert(detector.process(frame.data(), frame.size(), 2500) == MotionResult::Detected);

    // The detector permits at most one burst per ten seconds during activity.
    frame.fill(120);
    assert(detector.process(frame.data(), frame.size(), 3000) == MotionResult::Quiet);
    assert(detector.process(frame.data(), frame.size(), 3500) == MotionResult::Quiet);
    for (std::size_t i = 500; i < 750; ++i) frame[i] = 175;
    assert(detector.process(frame.data(), frame.size(), 11000) == MotionResult::Quiet);
    assert(detector.process(frame.data(), frame.size(), 11500) == MotionResult::Quiet);
    assert(detector.process(frame.data(), frame.size(), 12500) == MotionResult::Detected);

    assert(detector.process(frame.data(), frame.size(), 13000, false) == MotionResult::Suspended);
    assert(detector.process(frame.data(), frame.size(), 13500) == MotionResult::Baseline);
}

static void sensitivity_cases()
{
    std::array<std::uint8_t, kMotionPixels> frame{};
    frame.fill(100);
    std::array<std::uint8_t, kMotionPixels> changed = frame;
    for (std::size_t i = 0; i < 200; ++i) changed[i] = 150;

    MotionDetector medium;
    medium.set_sensitivity_level(2);
    assert(medium.process(frame.data(), frame.size(), 0) == MotionResult::Baseline);
    assert(medium.process(changed.data(), changed.size(), 500) == MotionResult::Quiet);
    assert(medium.process(changed.data(), changed.size(), 1000) == MotionResult::Detected);

    MotionDetector low;
    low.set_sensitivity_level(1);
    assert(low.process(frame.data(), frame.size(), 0) == MotionResult::Baseline);
    assert(low.process(changed.data(), changed.size(), 500) == MotionResult::Quiet);
    assert(low.process(changed.data(), changed.size(), 1000) == MotionResult::Quiet);

    MotionDetector high;
    high.set_sensitivity_level(3);
    assert(high.process(frame.data(), frame.size(), 0) == MotionResult::Baseline);
    assert(high.process(changed.data(), changed.size(), 500) == MotionResult::Quiet);
    assert(high.process(changed.data(), changed.size(), 1000) == MotionResult::Detected);
}

static void scan_safety_cases()
{
    assert(ScanController::valid_config({-1280, 0, 1280, 100}));
    assert(ScanController::valid_config({-450, 0, 450, 800}));
    assert(!ScanController::valid_config({-1281, 0, 1280, 450}));
    assert(!ScanController::valid_config({0, -100, 450, 450}));
    assert(!ScanController::valid_config({-450, 0, 450, 99}));
    assert(!ScanController::valid_config({-450, 0, 450, 801}));

    ScanController invalid_feedback;
    assert(invalid_feedback.start(0, {}));
    assert(invalid_feedback.initial_action().kind == ScanActionKind::Move);
    assert(invalid_feedback.update(20, {}) .kind == ScanActionKind::Failed);
    assert(invalid_feedback.failure()[0] != '\0');

    ScanController stalled;
    assert(stalled.start(0, {}));
    assert(stalled.update(0, {0, 450, true, true}).kind == ScanActionKind::None);
    assert(stalled.update(1500, {0, 450, true, true}).kind == ScanActionKind::Failed);
    assert(stalled.failure()[0] != '\0');

    ScanController timeout;
    assert(timeout.start(0, {}));
    assert(timeout.update(5000, {0, 450, false, true}).kind == ScanActionKind::Failed);
    assert(timeout.failure()[0] != '\0');

    ScanController cancelled;
    assert(cancelled.start(0, {}));
    assert(cancelled.cancel("Home requested").kind == ScanActionKind::Failed);
    assert(cancelled.stage() == ScanStage::Failed);
    assert(cancelled.failure()[0] == 'H');

    ScanController scan;
    assert(scan.start(0, {}));
    std::uint64_t now = 0;
    const std::array<int, 4> expected_yaws{-450, 0, 450, 0};
    for (std::size_t view = 0; view < expected_yaws.size(); ++view) {
        auto move = view == 0 ? scan.initial_action() : ScanAction{};
        if (view > 0) {
            // The previous successful capture produces the next move action.
            move = scan.capture_completed(now, true);
        }
        assert(move.kind == ScanActionKind::Move);
        assert(move.yaw == expected_yaws[view]);
        auto action = scan.update(now, {move.yaw, move.pitch, false, true});
        assert(action.kind == ScanActionKind::None);
        now += 1000;
        action = scan.update(now, {move.yaw, move.pitch, false, true});
        if (view < 3) {
            assert(action.kind == ScanActionKind::Capture);
            assert(action.view_index == view);
        } else {
            assert(action.kind == ScanActionKind::Complete);
            assert(scan.stage() == ScanStage::Complete);
        }
    }
    assert(scan.capture_completed(now, false).kind == ScanActionKind::None);
}

static void queue_budget_cases()
{
    assert(!queue_needs_oldest_drop(0, 0, 1));
    assert(!queue_needs_oldest_drop(kMaxPendingImages - 1, 900, 100));
    assert(queue_needs_oldest_drop(kMaxPendingImages, 0, 1));
    assert(!queue_needs_oldest_drop(1, kMaxPendingBytes - 100, 100));
    assert(queue_needs_oldest_drop(1, kMaxPendingBytes - 100, 101));
    assert(queue_needs_oldest_drop(0, 0, kMaxPendingBytes + 1));
}

static void schedule_and_drift_cases()
{
    ScanSchedule schedule;
    schedule.arm(1234);
    assert(schedule.due(1234)); // first scan starts when armed
    schedule.completed(2000);
    assert(!schedule.due(2000 + kScanIntervalMs - 1));
    assert(schedule.due(2000 + kScanIntervalMs));
    schedule.disarm();
    assert(!schedule.due(2000 + kScanIntervalMs));

    assert(!position_drifted({20, 450, false, true}, 0, 450));
    assert(position_drifted({21, 450, false, true}, 0, 450));
    assert(position_drifted({0, 471, false, true}, 0, 450));
    assert(position_drifted({0, 450, true, true}, 0, 450));
    assert(position_drifted({0, 450, false, false}, 0, 450));
}

int main()
{
    motion_detector_cases();
    sensitivity_cases();
    queue_budget_cases();
    scan_safety_cases();
    schedule_and_drift_cases();
}
