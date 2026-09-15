/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "sl_bus_departures.h"

#include <cJSON.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace sl_bus {
namespace {

const char* string_field(const cJSON* object, const char* name)
{
    if (!object || !cJSON_IsObject(object)) {
        return nullptr;
    }
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(value) && value->valuestring != nullptr ? value->valuestring : nullptr;
}

bool is_integer_field(const cJSON* object, const char* name, int expected)
{
    if (!object || !cJSON_IsObject(object)) {
        return false;
    }

    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, name);
    if (cJSON_IsNumber(value)) {
        return std::isfinite(value->valuedouble) && value->valuedouble == expected;
    }
    if (!cJSON_IsString(value) || value->valuestring == nullptr || value->valuestring[0] == '\0') {
        return false;
    }

    char* end = nullptr;
    const long parsed = std::strtol(value->valuestring, &end, 10);
    return end != value->valuestring && *end == '\0' && parsed == expected;
}

int integer_field(const cJSON* object, const char* name)
{
    if (!object || !cJSON_IsObject(object)) {
        return 0;
    }
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, name);
    if (cJSON_IsNumber(value) && std::isfinite(value->valuedouble)) {
        if (value->valuedouble > std::numeric_limits<int>::max()) {
            return std::numeric_limits<int>::max();
        }
        if (value->valuedouble < std::numeric_limits<int>::min()) {
            return std::numeric_limits<int>::min();
        }
        return static_cast<int>(value->valuedouble);
    }
    return 0;
}

bool equals_ignore_case(std::string_view lhs, std::string_view rhs)
{
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
            std::tolower(static_cast<unsigned char>(rhs[i]))) {
            return false;
        }
    }
    return true;
}

bool contains_ignore_case(std::string_view value, std::string_view needle)
{
    if (needle.empty() || value.size() < needle.size()) {
        return false;
    }
    for (std::size_t offset = 0; offset + needle.size() <= value.size(); ++offset) {
        if (equals_ignore_case(value.substr(offset, needle.size()), needle)) {
            return true;
        }
    }
    return false;
}

std::size_t utf8_sequence_length(unsigned char lead)
{
    if (lead <= 0x7F) {
        return 1;
    }
    if (lead >= 0xC2 && lead <= 0xDF) {
        return 2;
    }
    if (lead >= 0xE0 && lead <= 0xEF) {
        return 3;
    }
    if (lead >= 0xF0 && lead <= 0xF4) {
        return 4;
    }
    return 1;
}

void append_piece(std::string& output, std::string_view piece)
{
    if (piece.empty() || output.size() >= kMaxTextBytes) {
        return;
    }

    if (!output.empty()) {
        constexpr std::string_view separator = " · ";
        const auto available = kMaxTextBytes - output.size();
        if (available <= separator.size()) {
            return;
        }
        output.append(separator.substr(0, available));
    }

    const auto available = kMaxTextBytes - output.size();
    output.append(bounded_text(piece, available));
}

std::string deviation_text(const cJSON* value)
{
    std::string result;
    if (cJSON_IsString(value) && value->valuestring != nullptr) {
        return bounded_text(value->valuestring);
    }

    const auto append_object = [&result](const cJSON* object) {
        if (!cJSON_IsObject(object)) {
            return;
        }
        const char* message = string_field(object, "message");
        if (!message) {
            message = string_field(object, "description");
        }
        if (!message) {
            message = string_field(object, "text");
        }
        if (message) {
            append_piece(result, message);
        }
        if (const char* consequence = string_field(object, "consequence")) {
            append_piece(result, consequence);
        }
    };

    if (cJSON_IsObject(value)) {
        append_object(value);
    } else if (cJSON_IsArray(value)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, value)
        {
            if (cJSON_IsString(item) && item->valuestring != nullptr) {
                append_piece(result, item->valuestring);
            } else {
                append_object(item);
            }
            if (result.size() >= kMaxTextBytes) {
                break;
            }
        }
    }
    return result;
}

void parse_notices(const cJSON* root, StopDepartureData& data)
{
    const cJSON* notices = cJSON_GetObjectItemCaseSensitive(root, "stop_deviations");
    if (!cJSON_IsArray(notices)) {
        return;
    }

    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, notices)
    {
        if (data.notice_count >= kMaxNotices) {
            break;
        }
        if (!cJSON_IsObject(item)) {
            continue;
        }
        const char* message = string_field(item, "message");
        if (!message || message[0] == '\0') {
            message = string_field(item, "description");
        }
        if (!message || message[0] == '\0') {
            continue;
        }

        auto& notice = data.notices[data.notice_count++];
        notice.message = bounded_text(message);
        if (const char* consequence = string_field(item, "consequence")) {
            notice.consequence = bounded_text(consequence);
        }
        notice.importance = integer_field(item, "importance");
    }
}

bool is_cancelled(const cJSON* item)
{
    const char* state = string_field(item, "state");
    return state != nullptr && contains_ignore_case(state, "cancel");
}

}  // namespace

std::string bounded_text(std::string_view text, std::size_t max_bytes)
{
    if (max_bytes == 0 || text.empty()) {
        return {};
    }

    std::size_t length = std::min(text.size(), max_bytes);
    if (length < text.size() && length > 0) {
        // Only trim when the limit lands inside a code point. A complete
        // multi-byte character at the boundary must remain intact.
        std::size_t sequence_start = length - 1;
        while (sequence_start > 0 &&
               (static_cast<unsigned char>(text[sequence_start]) & 0xC0) == 0x80) {
            --sequence_start;
        }
        const auto sequence_length =
            utf8_sequence_length(static_cast<unsigned char>(text[sequence_start]));
        if (sequence_start + sequence_length > length) {
            length = sequence_start;
        }
    }
    return std::string(text.substr(0, length));
}

ParseResult parse_departures(std::string_view response, std::string_view line_designation, int direction_code)
{
    ParseResult result;
    if (response.empty()) {
        result.error = "empty response";
        return result;
    }
    if (response.size() > kMaxResponseBytes) {
        result.error = "response is too large";
        return result;
    }

    cJSON* root = cJSON_ParseWithLength(response.data(), response.size());
    if (!root) {
        result.error = "malformed JSON";
        return result;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        result.error = "response is not an object";
        return result;
    }

    const cJSON* departures = cJSON_GetObjectItemCaseSensitive(root, "departures");
    if (!cJSON_IsArray(departures)) {
        cJSON_Delete(root);
        result.error = "departures is not an array";
        return result;
    }

    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, departures)
    {
        if (result.data.departure_count >= kMaxDepartures) {
            break;
        }
        if (!cJSON_IsObject(item) || !is_integer_field(item, "direction_code", direction_code)) {
            continue;
        }

        const cJSON* line_object = cJSON_GetObjectItemCaseSensitive(item, "line");
        if (!cJSON_IsObject(line_object)) {
            continue;
        }
        const char* designation = string_field(line_object, "designation");
        const char* transport_mode = string_field(line_object, "transport_mode");
        if (!designation || std::string_view(designation) != line_designation || !transport_mode ||
            !equals_ignore_case(transport_mode, "BUS")) {
            continue;
        }

        const char* destination = string_field(item, "destination");
        if (!destination || destination[0] == '\0') {
            continue;
        }

        const bool cancelled = is_cancelled(item);
        const char* display = string_field(item, "display");
        if ((!display || display[0] == '\0') && !cancelled) {
            continue;
        }

        auto& departure = result.data.departures[result.data.departure_count++];
        departure.display = display && display[0] != '\0' ? bounded_text(display) : "Cancelled";
        departure.destination = bounded_text(destination);
        departure.cancelled = cancelled;

        const cJSON* deviations = cJSON_GetObjectItemCaseSensitive(item, "deviations");
        departure.disruption = deviation_text(deviations);
        if (departure.cancelled && departure.disruption.empty()) {
            departure.disruption = "Cancelled";
        }
    }

    parse_notices(root, result.data);
    cJSON_Delete(root);
    result.valid = true;
    return result;
}

ParseResult parse_departures(std::string_view response)
{
    return parse_departures(response, kStops[0].line, kStops[0].direction_code);
}

uint32_t parse_retry_after_seconds(std::string_view value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    if (value.empty()) {
        return 0;
    }

    uint64_t seconds = 0;
    for (const char character : value) {
        if (character < '0' || character > '9') {
            return 0;
        }
        const uint64_t digit = static_cast<uint64_t>(character - '0');
        if (seconds > (std::numeric_limits<uint32_t>::max() - digit) / 10) {
            return std::numeric_limits<uint32_t>::max();
        }
        seconds = seconds * 10 + digit;
    }
    return static_cast<uint32_t>(seconds);
}

uint32_t retry_delay_seconds(uint32_t consecutive_failures, uint32_t server_retry_after_seconds)
{
    uint32_t client_delay = 60;
    if (consecutive_failures == 2) {
        client_delay = 120;
    } else if (consecutive_failures >= 3) {
        client_delay = 300;
    }
    return std::max(client_delay, server_retry_after_seconds);
}

void DepartureStore::reset()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _snapshot = {};
}

void DepartureStore::set_fetching(std::size_t stop_index)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (stop_index >= kStopCount) {
        return;
    }
    auto& stop = _snapshot.stops[stop_index];
    stop.fetch_status = FetchStatus::Fetching;
    stop.error.clear();
    ++_snapshot.generation;
}

void DepartureStore::publish_success(std::size_t stop_index, const StopDepartureData& data, uint64_t now_ms)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (stop_index >= kStopCount) {
        return;
    }
    auto& stop = _snapshot.stops[stop_index];
    stop.departures = {};
    stop.departure_count = std::min(data.departure_count, kMaxDepartures);
    for (std::size_t i = 0; i < stop.departure_count; ++i) {
        stop.departures[i].display = bounded_text(data.departures[i].display);
        stop.departures[i].destination = bounded_text(data.departures[i].destination);
        stop.departures[i].disruption = bounded_text(data.departures[i].disruption);
        stop.departures[i].cancelled = data.departures[i].cancelled;
    }
    stop.notices = {};
    stop.notice_count = std::min(data.notice_count, kMaxNotices);
    for (std::size_t i = 0; i < stop.notice_count; ++i) {
        stop.notices[i].message = bounded_text(data.notices[i].message);
        stop.notices[i].consequence = bounded_text(data.notices[i].consequence);
        stop.notices[i].importance = data.notices[i].importance;
    }
    stop.fetch_status = stop.departure_count == 0 ? FetchStatus::Empty : FetchStatus::Success;
    stop.error.clear();
    stop.last_success_ms = now_ms;
    stop.expired = false;
    ++_snapshot.generation;
}

void DepartureStore::publish_failure(std::size_t stop_index, std::string_view error)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (stop_index >= kStopCount) {
        return;
    }
    auto& stop = _snapshot.stops[stop_index];
    stop.fetch_status = FetchStatus::Failed;
    stop.error = bounded_text(error);
    ++_snapshot.generation;
}

DepartureSnapshot DepartureStore::snapshot(uint64_t now_ms) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto result = _snapshot;
    for (auto& stop : result.stops) {
        stop.expired = stop.last_success_ms == 0 ||
                       (now_ms >= stop.last_success_ms &&
                        now_ms - stop.last_success_ms >= kDataExpiryMs);
    }
    return result;
}

}  // namespace sl_bus
