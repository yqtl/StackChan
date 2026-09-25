#include "sl_bus_close_state.hpp"
#include "sl_bus_departure_client.h"
#include "sl_bus_poll_schedule.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace allocation_tracking {
bool enabled = false;
std::size_t allocations = 0;
}  // namespace allocation_tracking

void* operator new(std::size_t size)
{
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        if (allocation_tracking::enabled) {
            ++allocation_tracking::allocations;
        }
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace {

using namespace sl_bus;

constexpr const char* kEmptyResponse = R"json({"departures":[]})json";

struct HttpScript {
    bool open = true;
    int status = 200;
    std::string retry_after;
    std::string body = kEmptyResponse;
    std::vector<std::string> chunks;
    int read_result = 0;
    std::size_t advertised_length = std::numeric_limits<std::size_t>::max();
};

struct HttpStats {
    std::size_t close_count = 0;
    std::size_t open_count = 0;
    std::size_t read_count = 0;
};

class ScriptedHttp final : public Http {
public:
    ScriptedHttp(HttpScript script, std::shared_ptr<HttpStats> stats)
        : _script(std::move(script)), _stats(std::move(stats))
    {
    }

    ~ScriptedHttp() override = default;

    void SetTimeout(int) override {}
    void SetHeader(const std::string&, const std::string&) override {}
    void SetContent(std::string&&) override {}
    void SetKeepAlive(bool) override {}

    bool Open(const std::string&, const std::string&) override
    {
        ++_stats->open_count;
        return _script.open;
    }

    void Close() override
    {
        ++_stats->close_count;
    }

    int Read(char* buffer, size_t buffer_size) override
    {
        ++_stats->read_count;
        if (_script.read_result != 0) {
            return _script.read_result;
        }

        if (!_script.chunks.empty()) {
            if (_chunk_index >= _script.chunks.size()) {
                return 0;
            }
            const auto& chunk = _script.chunks[_chunk_index++];
            const auto count = std::min(buffer_size, chunk.size());
            std::memcpy(buffer, chunk.data(), count);
            return static_cast<int>(count);
        }

        if (_body_offset >= _script.body.size()) {
            return 0;
        }
        const auto count = std::min(buffer_size, _script.body.size() - _body_offset);
        std::memcpy(buffer, _script.body.data() + _body_offset, count);
        _body_offset += count;
        return static_cast<int>(count);
    }

    int Write(const char*, size_t) override { return 0; }
    int GetStatusCode() override { return _script.status; }

    std::string GetResponseHeader(const std::string& key) const override
    {
        return key == "Retry-After" ? _script.retry_after : std::string{};
    }

    size_t GetBodyLength() override
    {
        return _script.advertised_length == std::numeric_limits<std::size_t>::max()
                   ? _script.body.size()
                   : _script.advertised_length;
    }

    std::string ReadAll() override { return {}; }
    int GetLastError() override { return 0; }

private:
    HttpScript _script;
    std::shared_ptr<HttpStats> _stats;
    std::size_t _body_offset = 0;
    std::size_t _chunk_index = 0;
};

struct ScriptedFactory {
    std::vector<HttpScript> scripts;
    std::shared_ptr<HttpStats> stats = std::make_shared<HttpStats>();
    std::size_t next_script = 0;

    std::unique_ptr<Http> operator()()
    {
        if (next_script >= scripts.size()) {
            return nullptr;
        }
        return std::make_unique<ScriptedHttp>(scripts[next_script++], stats);
    }
};

FetchResult failure(FetchFailureKind kind, int status = 0, uint32_t retry_after = 0)
{
    FetchResult result;
    result.status = FetchResultStatus::Failed;
    result.failure_kind = kind;
    result.http_status = status;
    result.retry_after_seconds = retry_after;
    result.error = "failure";
    return result;
}

void test_client_retry_and_cancellation()
{
    bool cancelled = false;
    ScriptedFactory retry_factory;
    HttpScript retry_failure;
    retry_failure.open = false;
    HttpScript retry_success;
    retry_success.body = kEmptyResponse;
    retry_factory.scripts = {retry_failure, retry_success};
    auto retry_stats = retry_factory.stats;
    DepartureClient retry_client([&cancelled]() { return cancelled; }, [&retry_factory]() {
        return retry_factory();
    });
    std::vector<uint32_t> delays;
    const auto retry_result = retry_client.fetch_with_retry(
        kStops[0], [&delays](uint32_t delay_ms) {
            delays.push_back(delay_ms);
            return false;
        });
    assert(retry_result.status == FetchResultStatus::Success);
    assert(retry_factory.next_script == 2);
    assert(delays == std::vector<uint32_t>{250});
    assert(retry_stats->close_count == 2);

    ScriptedFactory double_failure_factory;
    double_failure_factory.scripts = {retry_failure, retry_failure};
    auto double_failure_stats = double_failure_factory.stats;
    DepartureClient double_failure_client([]() { return false; }, [&double_failure_factory]() {
        return double_failure_factory();
    });
    delays.clear();
    const auto double_failure = double_failure_client.fetch_with_retry(
        kStops[0], [&delays](uint32_t delay_ms) {
            delays.push_back(delay_ms);
            return false;
        });
    assert(double_failure.status == FetchResultStatus::Failed);
    assert(double_failure.failure_kind == FetchFailureKind::Transport);
    assert(double_failure_factory.next_script == 2);
    assert(delays == std::vector<uint32_t>{250});
    assert(double_failure_stats->close_count == 2);

    for (const int status : {404, 429, 503}) {
        ScriptedFactory status_factory;
        HttpScript script;
        script.status = status;
        status_factory.scripts.push_back(script);
        auto status_stats = status_factory.stats;
        DepartureClient status_client([]() { return false; }, [&status_factory]() {
            return status_factory();
        });
        delays.clear();
        const auto result = status_client.fetch_with_retry(
            kStops[0], [&delays](uint32_t delay_ms) {
                delays.push_back(delay_ms);
                return false;
            });
        assert(result.status == FetchResultStatus::Failed);
        assert(result.failure_kind == FetchFailureKind::HttpStatus);
        assert(result.http_status == status);
        assert(delays.empty());
        assert(status_stats->close_count == 1);
    }

    ScriptedFactory malformed_factory;
    HttpScript malformed_script;
    malformed_script.body = "not json";
    malformed_factory.scripts = {malformed_script};
    auto malformed_stats = malformed_factory.stats;
    DepartureClient malformed_client([]() { return false; }, [&malformed_factory]() {
        return malformed_factory();
    });
    delays.clear();
    const auto malformed = malformed_client.fetch_with_retry(
        kStops[0], [&delays](uint32_t delay_ms) {
            delays.push_back(delay_ms);
            return false;
        });
    assert(malformed.failure_kind == FetchFailureKind::InvalidResponse);
    assert(delays.empty());
    assert(malformed_stats->close_count == 1);

    ScriptedFactory oversized_factory;
    HttpScript oversized;
    oversized.advertised_length = kMaxResponseBytes + 1;
    oversized_factory.scripts.push_back(oversized);
    auto oversized_stats = oversized_factory.stats;
    DepartureClient oversized_client([]() { return false; }, [&oversized_factory]() {
        return oversized_factory();
    });
    const auto oversized_result = oversized_client.fetch_with_retry(kStops[0], {});
    assert(oversized_result.failure_kind == FetchFailureKind::InvalidResponse);
    assert(oversized_stats->close_count == 1);

    ScriptedFactory read_error_factory;
    HttpScript read_error;
    read_error.read_result = -1;
    read_error_factory.scripts.push_back(read_error);
    auto read_error_stats = read_error_factory.stats;
    DepartureClient read_error_client([]() { return false; }, [&read_error_factory]() {
        return read_error_factory();
    });
    const auto read_error_result = read_error_client.fetch(kStops[0]);
    assert(read_error_result.failure_kind == FetchFailureKind::Transport);
    assert(read_error_stats->close_count == 1);

    ScriptedFactory invalid_status_factory;
    HttpScript invalid_status;
    invalid_status.status = 0;
    invalid_status.retry_after = "600";
    invalid_status_factory.scripts.push_back(invalid_status);
    auto invalid_status_stats = invalid_status_factory.stats;
    DepartureClient invalid_status_client([]() { return false; }, [&invalid_status_factory]() {
        return invalid_status_factory();
    });
    delays.clear();
    const auto invalid_status_result = invalid_status_client.fetch_with_retry(
        kStops[0], [&delays](uint32_t delay_ms) {
            delays.push_back(delay_ms);
            return false;
        });
    assert(invalid_status_result.failure_kind == FetchFailureKind::Transport);
    assert(invalid_status_result.http_status == 0);
    assert(invalid_status_result.retry_after_seconds == 600);
    assert(delays.empty());
    assert(invalid_status_stats->close_count == 1);

    DepartureClient null_factory_client([]() { return false; }, []() { return std::unique_ptr<Http>{}; });
    const auto null_factory = null_factory_client.fetch_with_retry(kStops[0], {});
    assert(null_factory.failure_kind == FetchFailureKind::Resource);

    ScriptedFactory retry_after_factory;
    HttpScript retry_after;
    retry_after.status = 503;
    retry_after.retry_after = "600";
    retry_after_factory.scripts.push_back(retry_after);
    DepartureClient retry_after_client([]() { return false; }, [&retry_after_factory]() {
        return retry_after_factory();
    });
    delays.clear();
    const auto retry_after_result = retry_after_client.fetch_with_retry(
        kStops[0], [&delays](uint32_t delay_ms) {
            delays.push_back(delay_ms);
            return false;
        });
    assert(retry_after_result.retry_after_seconds == 600);
    assert(retry_after_result.http_status == 503);
    assert(delays.empty());

    std::size_t cancelled_factory_calls = 0;
    DepartureClient cancelled_before_request(
        []() { return true; }, [&cancelled_factory_calls]() {
            ++cancelled_factory_calls;
            return std::unique_ptr<Http>{};
        });
    assert(cancelled_before_request.fetch_with_retry(kStops[0], {}).status == FetchResultStatus::Cancelled);
    assert(cancelled_factory_calls == 0);

    bool cancel_during_wait = false;
    ScriptedFactory wait_cancel_factory;
    wait_cancel_factory.scripts = {retry_failure};
    DepartureClient wait_cancel_client([&cancel_during_wait]() { return cancel_during_wait; },
                                       [&wait_cancel_factory]() { return wait_cancel_factory(); });
    const auto wait_cancel = wait_cancel_client.fetch_with_retry(
        kStops[0], [&cancel_during_wait](uint32_t delay_ms) {
            assert(delay_ms == 250);
            cancel_during_wait = true;
            return true;
        });
    assert(wait_cancel.status == FetchResultStatus::Cancelled);
    assert(wait_cancel_factory.next_script == 1);

    bool cancel_between_reads = false;
    ScriptedFactory body_cancel_factory_2;
    HttpScript body_cancel_2;
    body_cancel_2.chunks = {"{", "\"departures\":[]}"};
    body_cancel_factory_2.scripts.push_back(body_cancel_2);
    auto body_cancel_stats_2 = body_cancel_factory_2.stats;
    DepartureClient body_cancel_client_2([&cancel_between_reads, &body_cancel_stats_2]() {
        if (body_cancel_stats_2->read_count > 0) {
            cancel_between_reads = true;
        }
        return cancel_between_reads;
    }, [&body_cancel_factory_2]() { return body_cancel_factory_2(); });
    const auto body_cancel_result_2 = body_cancel_client_2.fetch(kStops[0]);
    assert(body_cancel_result_2.status == FetchResultStatus::Cancelled);
    assert(body_cancel_stats_2->close_count == 1);
}

void test_schedule()
{
    PollSchedule schedule;
    schedule.begin_session(0);
    assert(schedule.next_due(0).value() == 0);
    schedule.record_success(0, 0);
    assert(schedule.next_due(0).value() == 1);
    schedule.record_success(1, 0);
    assert(!schedule.next_due(0));
    assert(schedule.next_wakeup_ms() == 30'000);

    const auto ordinary = failure(FetchFailureKind::Transport);
    PollSchedule independent;
    independent.begin_session(0);
    independent.record_failure(0, ordinary, 0);
    independent.record_success(1, 0);
    assert(independent.next_due(30'000).value() == 1);
    independent.record_success(1, 30'000);
    assert(independent.next_due(60'000).value() == 0);
    independent.record_failure(0, ordinary, 60'000);
    assert(independent.next_due(60'000).value() == 1);
    independent.record_success(1, 60'000);
    independent.record_success(1, 90'000);
    independent.record_success(1, 120'000);
    independent.record_success(1, 150'000);
    assert(independent.next_due(180'000).value() == 0);
    independent.record_failure(0, ordinary, 180'000);
    // The third ordinary failure uses the 300-second saturated backoff.
    independent.record_success(1, 180'000);
    independent.record_success(1, 210'000);
    independent.record_success(1, 240'000);
    independent.record_success(1, 270'000);
    independent.record_success(1, 300'000);
    independent.record_success(1, 330'000);
    independent.record_success(1, 360'000);
    independent.record_success(1, 390'000);
    independent.record_success(1, 420'000);
    independent.record_success(1, 450'000);
    assert(independent.next_due(480'000).value() == 0);
    independent.record_failure(0, ordinary, 480'000);
    independent.record_success(1, 480'000);
    independent.record_success(1, 510'000);
    independent.record_success(1, 540'000);
    independent.record_success(1, 570'000);
    independent.record_success(1, 600'000);
    independent.record_success(1, 630'000);
    independent.record_success(1, 660'000);
    independent.record_success(1, 690'000);
    independent.record_success(1, 720'000);
    independent.record_success(1, 750'000);
    assert(independent.next_due(780'000).value() == 0);

    FetchResult throttle = failure(FetchFailureKind::HttpStatus, 429);
    PollSchedule cooldown;
    cooldown.begin_session(0);
    cooldown.record_failure(0, throttle, 0);
    assert(cooldown.cooldown_active(0));
    assert(!cooldown.next_due(0));
    assert(cooldown.next_wakeup_ms() == 60'000);
    cooldown.begin_session(10'000);
    assert(cooldown.cooldown_active(10'000));
    assert(!cooldown.next_due(10'000));
    assert(cooldown.next_wakeup_ms() == 60'000);
    cooldown.record_failure(1, throttle, 10'000);
    assert(cooldown.next_wakeup_ms() == 130'000);
    cooldown.record_success(0, 20'000);
    assert(cooldown.next_wakeup_ms() == 130'000);

    PollSchedule retry_after_schedule;
    retry_after_schedule.begin_session(0);
    retry_after_schedule.record_failure(0, failure(FetchFailureKind::HttpStatus, 503, 600), 0);
    assert(retry_after_schedule.next_wakeup_ms() == 600'000);
    retry_after_schedule.begin_session(1'000);
    assert(!retry_after_schedule.next_due(1'000));
    assert(retry_after_schedule.next_wakeup_ms() == 600'000);
    assert(retry_after_schedule.next_due(600'000).value() == 0);

    PollSchedule ordinary_503;
    ordinary_503.begin_session(0);
    ordinary_503.record_failure(0, failure(FetchFailureKind::HttpStatus, 503), 0);
    assert(ordinary_503.next_due(0).value() == 1);
    assert(ordinary_503.next_wakeup_ms() == 0);

    PollSchedule saturated;
    saturated.begin_session(0);
    saturated.record_failure(0, failure(FetchFailureKind::HttpStatus, 503, UINT32_MAX), 0);
    assert(saturated.next_wakeup_ms() == static_cast<uint64_t>(UINT32_MAX) * 1000ULL);
}

void test_close_state()
{
    // The close helper is intentionally host-only here; real RTOS semaphore
    // timing and modem/TLS cancellation remain firmware/hardware checks.
    SlBusCloseState state;
    assert(state.request_home() == SlBusCloseState::Action::Stop);
    assert(state.request_home() == SlBusCloseState::Action::None);
    assert(state.worker_completed() == SlBusCloseState::Action::Close);
    assert(state.worker_completed() == SlBusCloseState::Action::None);

    state.reset();
    assert(state.worker_completed() == SlBusCloseState::Action::None);
    assert(state.request_home() == SlBusCloseState::Action::Stop);
    assert(state.worker_completed() == SlBusCloseState::Action::Close);

    for (int cycle = 0; cycle < 10; ++cycle) {
        state.reset();
        assert(state.request_home() == SlBusCloseState::Action::Stop);
        assert(state.worker_completed() == SlBusCloseState::Action::Close);
        assert(state.worker_completed() == SlBusCloseState::Action::None);
    }
}

void test_snapshot_copy_and_expiry()
{
    DepartureStore store;
    DepartureSnapshot cache;
    assert(store.copy_if_changed(cache, true));

    StopDepartureData data;
    data.notice_count = 1;
    data.notices[0].message.assign(300, 'n');
    data.notices[0].consequence.assign(300, 'c');
    data.departure_count = 1;
    data.departures[0].display = "5 min";
    data.departures[0].destination = "Hjorthagen";

    store.publish_success(0, data, 1'000);
    assert(store.copy_if_changed(cache));
    assert(cache.stops[0].fetch_status == FetchStatus::Success);
    assert(cache.stops[0].notices[0].message.size() == kMaxTextBytes);

    cache.stops[0].notices[0].message = "sentinel";
    allocation_tracking::enabled = true;
    const auto allocations_before = allocation_tracking::allocations;
    assert(!store.copy_if_changed(cache));
    assert(allocation_tracking::allocations == allocations_before);
    allocation_tracking::enabled = false;
    assert(cache.stops[0].notices[0].message == "sentinel");

    store.set_fetching(0);
    assert(store.copy_if_changed(cache));
    assert(cache.stops[0].fetch_status == FetchStatus::Fetching);
    store.publish_failure(0, "request failed");
    assert(store.copy_if_changed(cache));
    assert(cache.stops[0].fetch_status == FetchStatus::Failed);
    store.publish_success(0, data, 1'000);
    assert(store.copy_if_changed(cache));

    store.update_expiry(cache, 90'999);
    assert(!cache.stops[0].expired);
    assert(!store.copy_if_changed(cache));
    store.update_expiry(cache, 91'000);
    assert(cache.stops[0].expired);
    store.update_expiry(cache, 500);
    assert(!cache.stops[0].expired);

    store.publish_success(1, StopDepartureData{}, 200'000);
    assert(store.copy_if_changed(cache));
    assert(cache.stops[1].fetch_status == FetchStatus::Empty);
    assert(cache.stops[0].last_success_ms == 1'000);
    store.update_expiry(cache, 200'001);
    assert(cache.stops[0].expired);
    assert(!cache.stops[1].expired);

    store.reset();
    assert(store.copy_if_changed(cache));
    assert(cache.generation == 0);
    const auto generation_zero = cache.generation;
    assert(!store.copy_if_changed(cache));
    assert(store.copy_if_changed(cache, true));
    assert(cache.generation == generation_zero);
}

}  // namespace

int main()
{
    test_client_retry_and_cancellation();
    test_schedule();
    test_close_state();
    test_snapshot_copy_and_expiry();
    std::cout << "PASS: SL client retry, independent schedule, close state and snapshot cache\n";
}
