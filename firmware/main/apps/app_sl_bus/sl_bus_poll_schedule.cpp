/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "sl_bus_poll_schedule.h"

#include <algorithm>
#include <limits>

namespace sl_bus {

void PollSchedule::begin_session(uint64_t now_ms)
{
    _failure_counts.fill(0);
    _next_due_ms.fill(now_ms);

    if (_shared_cooldown_deadline_ms <= now_ms) {
        _shared_cooldown_deadline_ms = 0;
        _throttle_failure_count = 0;
    }
}

std::optional<std::size_t> PollSchedule::next_due(uint64_t now_ms) const
{
    if (cooldown_active(now_ms)) {
        return std::nullopt;
    }

    for (std::size_t stop = 0; stop < kStopCount; ++stop) {
        if (_next_due_ms[stop] <= now_ms) {
            return stop;
        }
    }
    return std::nullopt;
}

uint64_t PollSchedule::next_wakeup_ms() const
{
    const auto earliest = *std::min_element(_next_due_ms.begin(), _next_due_ms.end());
    return std::max(earliest, _shared_cooldown_deadline_ms);
}

void PollSchedule::record_success(std::size_t stop, uint64_t completed_ms)
{
    if (stop >= kStopCount) {
        return;
    }
    _failure_counts[stop] = 0;
    _next_due_ms[stop] = add_seconds(completed_ms, kPollIntervalSeconds);
    _throttle_failure_count = 0;
}

void PollSchedule::record_failure(std::size_t stop, const FetchResult& result, uint64_t completed_ms)
{
    if (stop >= kStopCount || result.status != FetchResultStatus::Failed) {
        return;
    }

    const bool shared_cooldown = result.http_status == 429 || result.retry_after_seconds > 0;
    if (shared_cooldown) {
        _throttle_failure_count = increment_failure_count(_throttle_failure_count);
        const auto delay_seconds =
            retry_delay_seconds(_throttle_failure_count, result.retry_after_seconds);
        _shared_cooldown_deadline_ms =
            std::max(_shared_cooldown_deadline_ms, add_seconds(completed_ms, delay_seconds));
        return;
    }

    _failure_counts[stop] = increment_failure_count(_failure_counts[stop]);
    _next_due_ms[stop] = add_seconds(
        completed_ms, retry_delay_seconds(_failure_counts[stop]));
}

bool PollSchedule::cooldown_active(uint64_t now_ms) const
{
    return _shared_cooldown_deadline_ms > now_ms;
}

uint64_t PollSchedule::add_seconds(uint64_t base_ms, uint32_t seconds)
{
    const uint64_t delay_ms = static_cast<uint64_t>(seconds) * 1000ULL;
    if (std::numeric_limits<uint64_t>::max() - base_ms < delay_ms) {
        return std::numeric_limits<uint64_t>::max();
    }
    return base_ms + delay_ms;
}

uint32_t PollSchedule::increment_failure_count(uint32_t count)
{
    return std::min<uint32_t>(count + (count < 3 ? 1 : 0), 3);
}

}  // namespace sl_bus
