#include "deferred_app_launcher.hpp"

#include <cassert>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

class RecordingApp : public mooncake::AppAbility {
public:
    RecordingApp(const char *name, std::vector<std::string> &events)
        : _events(events)
    {
        setAppInfo().name = name;
        _name = name;
    }

    void onOpen() override { _events.push_back(_name + ":open"); }
    void onClose() override { _events.push_back(_name + ":close"); }

private:
    std::vector<std::string> &_events;
    std::string _name;
};

class TestLauncher : public DeferredAppLauncher {};

void pump(unsigned count = 1)
{
    for (unsigned index = 0; index < count; ++index) mooncake::GetMooncake().update();
}

template <typename Predicate>
void pump_until(Predicate predicate)
{
    for (unsigned index = 0; index < 40 && !predicate(); ++index) pump();
    assert(predicate());
}

std::size_t event_index(const std::vector<std::string> &events, const std::string &event)
{
    for (std::size_t index = 0; index < events.size(); ++index) {
        if (events[index] == event) return index;
    }
    return events.size();
}

void run_order_test(bool source_before_launcher)
{
    mooncake::DestroyMooncake();
    std::vector<std::string> events;

    auto launcher = std::make_unique<TestLauncher>();
    auto *launcher_ptr = launcher.get();
    auto source = std::make_unique<RecordingApp>("gesture", events);
    auto *source_ptr = source.get();
    auto target = std::make_unique<RecordingApp>("sl", events);
    auto *target_ptr = target.get();

    int source_id = -1;
    int launcher_id = -1;
    int target_id = -1;
    if (source_before_launcher) {
        source_id = mooncake::GetMooncake().installApp(std::move(source));
        launcher_id = mooncake::GetMooncake().installApp(std::move(launcher));
        target_id = mooncake::GetMooncake().installApp(std::move(target));
    } else {
        launcher_id = mooncake::GetMooncake().installApp(std::move(launcher));
        source_id = mooncake::GetMooncake().installApp(std::move(source));
        target_id = mooncake::GetMooncake().installApp(std::move(target));
    }
    assert(launcher_id >= 0 && source_id >= 0 && target_id >= 0);

    launcher_ptr->open();
    pump_until([&]() { return launcher_ptr->currentState() == mooncake::AppAbility::StateRunning; });

    // Manual Launcher -> Gesture still uses the base launch path.
    assert(launcher_ptr->openApp(source_id));
    pump_until([&]() { return source_ptr->currentState() == mooncake::AppAbility::StateRunning; });
    assert(launcher_ptr->getRunningAppId() == source_id);

    // Invalid requests do not alter launcher state.
    assert(!launcher_ptr->requestAppAfterClose(source_id, source_id));
    assert(!launcher_ptr->requestAppAfterClose(source_id, target_id + 1000));
    source_ptr->close();
    assert(!launcher_ptr->requestAppAfterClose(source_id, target_id));
    pump_until([&]() {
        return launcher_ptr->currentState() == mooncake::AppAbility::StateRunning &&
               launcher_ptr->getRunningAppId() < 0;
    });

    // Target already running is rejected while Gesture remains the tracked
    // foreground app.
    assert(launcher_ptr->openApp(source_id));
    pump_until([&]() { return source_ptr->currentState() == mooncake::AppAbility::StateRunning; });
    target_ptr->open();
    pump_until([&]() { return target_ptr->currentState() == mooncake::AppAbility::StateRunning; });
    assert(launcher_ptr->getRunningAppId() == source_id);
    assert(!launcher_ptr->requestAppAfterClose(source_id, target_id));
    target_ptr->close();
    pump_until([&]() { return target_ptr->currentState() == mooncake::AppAbility::StateSleeping; });

    // One accepted request is latched. A duplicate cannot replace it, and SL
    // is not opened until Gesture's close callback has run.
    events.clear();
    assert(launcher_ptr->requestAppAfterClose(source_id, target_id));
    assert(!launcher_ptr->requestAppAfterClose(source_id, target_id));
    source_ptr->close();
    assert(event_index(events, "sl:open") == events.size());
    pump_until([&]() { return target_ptr->currentState() == mooncake::AppAbility::StateRunning; });
    assert(event_index(events, "gesture:close") < event_index(events, "sl:open"));

    // Closing SL reopens Launcher with its running ID cleared.
    target_ptr->close();
    pump_until([&]() {
        return launcher_ptr->currentState() == mooncake::AppAbility::StateRunning &&
               launcher_ptr->getRunningAppId() < 0;
    });

    // Exercise repeated Gesture -> SL -> Home cycles, including the manual
    // Gesture -> Home leg at every boundary.
    for (int cycle = 0; cycle < 10; ++cycle) {
        events.clear();
        assert(launcher_ptr->openApp(source_id));
        pump_until([&]() { return source_ptr->currentState() == mooncake::AppAbility::StateRunning; });
        assert(launcher_ptr->requestAppAfterClose(source_id, target_id));
        source_ptr->close();
        pump_until([&]() { return target_ptr->currentState() == mooncake::AppAbility::StateRunning; });
        assert(event_index(events, "gesture:close") < event_index(events, "sl:open"));
        target_ptr->close();
        pump_until([&]() {
            return launcher_ptr->currentState() == mooncake::AppAbility::StateRunning &&
                   launcher_ptr->getRunningAppId() < 0;
        });
    }

    // A pending request is discarded and Launcher recovers if its source is
    // uninstalled before the source can finish closing.
    assert(launcher_ptr->openApp(source_id));
    pump_until([&]() { return source_ptr->currentState() == mooncake::AppAbility::StateRunning; });
    assert(launcher_ptr->requestAppAfterClose(source_id, target_id));
    assert(mooncake::GetMooncake().uninstallApp(source_id));
    pump_until([&]() {
        return launcher_ptr->currentState() == mooncake::AppAbility::StateRunning &&
               launcher_ptr->getRunningAppId() < 0;
    });
    assert(target_ptr->currentState() == mooncake::AppAbility::StateSleeping);
}

}  // namespace

int main()
{
    // Both installation orders are valid because the deferred decision is
    // made from the launcher's later onSleeping callback.
    run_order_test(false);
    run_order_test(true);
    mooncake::DestroyMooncake();
    std::cout << "PASS: deferred launcher lifecycle, rejection and ten-cycle recovery tests\n";
}
