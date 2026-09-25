/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "sl_bus_departure_client.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace sl_bus {

class PollSchedule {
public:
    void begin_session(uint64_t now_ms);
    std::optional<std::size_t> next_due(uint64_t now_ms) const;
    uint64_t next_wakeup_ms() const;
    void record_success(std::size_t stop, uint64_t completed_ms);
    void record_failure(std::size_t stop, const FetchResult& result, uint64_t completed_ms);

    bool cooldown_active(uint64_t now_ms) const;

private:
    static uint64_t add_seconds(uint64_t base_ms, uint32_t seconds);
    static uint32_t increment_failure_count(uint32_t count);

    std::array<uint32_t, kStopCount> _failure_counts{};
    std::array<uint64_t, kStopCount> _next_due_ms{};
    uint64_t _shared_cooldown_deadline_ms = 0;
    uint32_t _throttle_failure_count = 0;
};

}  // namespace sl_bus
