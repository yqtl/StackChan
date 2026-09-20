/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace sl_bus {

inline constexpr std::size_t kStopCount = 2;

struct StopConfig {
    std::string_view name;
    std::string_view line;
    std::string_view destination;
    std::string_view url;
};

inline constexpr std::array<StopConfig, kStopCount> kStops = {{
    {"Tullgårdsparken", "57",
     "Hjorthagen",
     "https://transport.integration.sl.se/v1/sites/1314/departures?transport=BUS&forecast=60"},
    {"Bohusgatan", "74",
     "Hornsberg",
     "https://transport.integration.sl.se/v1/sites/1318/departures?transport=BUS&forecast=60"},
}};

inline constexpr std::size_t kMaxDepartures = 3;
inline constexpr std::size_t kMaxNotices = 3;
inline constexpr std::size_t kMaxTextBytes = 160;
inline constexpr std::size_t kMaxResponseBytes = 32 * 1024;
inline constexpr uint32_t kDataExpiryMs = 90 * 1000;
inline constexpr uint32_t kPollIntervalSeconds = 30;
inline constexpr uint32_t kHttpTimeoutMs = 5 * 1000;

struct Departure {
    std::string display;
    std::string destination;
    std::string disruption;
    bool cancelled = false;
};

struct Notice {
    std::string message;
    std::string consequence;
    int importance = 0;
};

struct StopDepartureData {
    std::array<Departure, kMaxDepartures> departures{};
    std::size_t departure_count = 0;
    std::array<Notice, kMaxNotices> notices{};
    std::size_t notice_count = 0;
};

enum class FetchStatus {
    Loading,
    Fetching,
    Success,
    Empty,
    Failed,
};

struct StopSnapshot : StopDepartureData {
    FetchStatus fetch_status = FetchStatus::Loading;
    std::string error;
    uint64_t last_success_ms = 0;
    bool expired = true;
};

struct DepartureSnapshot {
    std::array<StopSnapshot, kStopCount> stops{};
    uint32_t generation = 0;
};

struct ParseResult {
    bool valid = false;
    StopDepartureData data;
    std::string error;
};

// Keep text bounded without cutting a UTF-8 code point in half.
std::string bounded_text(std::string_view text, std::size_t max_bytes = kMaxTextBytes);

ParseResult parse_departures(std::string_view response, std::string_view line, std::string_view destination);
ParseResult parse_departures(std::string_view response);

// Retry-After's delta-seconds form is usable even when SNTP has not completed.
uint32_t parse_retry_after_seconds(std::string_view value);

// consecutive_failures is one-based. A server delay can only lengthen the
// client backoff, never shorten it.
uint32_t retry_delay_seconds(uint32_t consecutive_failures, uint32_t server_retry_after_seconds = 0);

class DepartureStore {
public:
    void reset();
    void set_fetching(std::size_t stop_index);
    void publish_success(std::size_t stop_index, const StopDepartureData& data, uint64_t now_ms);
    void publish_failure(std::size_t stop_index, std::string_view error);
    DepartureSnapshot snapshot(uint64_t now_ms) const;

private:
    mutable std::mutex _mutex;
    DepartureSnapshot _snapshot;
};

}  // namespace sl_bus
