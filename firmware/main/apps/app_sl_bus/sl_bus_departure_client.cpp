/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "sl_bus_departure_client.h"

#include <array>
#include <algorithm>
#include <memory>
#include <utility>

namespace sl_bus {
namespace {

FetchResult failed_result(std::string_view error,
                          FetchFailureKind failure_kind,
                          int http_status = 0,
                          uint32_t retry_after = 0)
{
    FetchResult result;
    result.status = FetchResultStatus::Failed;
    result.error = bounded_text(error);
    result.retry_after_seconds = retry_after;
    result.failure_kind = failure_kind;
    result.http_status = http_status;
    return result;
}

FetchResult cancelled_result()
{
    FetchResult result;
    result.status = FetchResultStatus::Cancelled;
    return result;
}

class HttpCloseGuard {
public:
    explicit HttpCloseGuard(Http* http) : _http(http) {}

    ~HttpCloseGuard() { close(); }

    void close()
    {
        if (_http && !_closed) {
            _http->Close();
            _closed = true;
        }
    }

private:
    Http* _http;
    bool _closed = false;
};

}  // namespace

DepartureClient::DepartureClient(CancelPredicate cancelled, HttpFactory make_http)
    : _cancelled(std::move(cancelled)), _make_http(std::move(make_http))
{
}

FetchResult DepartureClient::fetch(const StopConfig& stop) const
{
    if (_cancelled && _cancelled()) {
        return cancelled_result();
    }

    if (!_make_http) {
        return failed_result("HTTP client unavailable", FetchFailureKind::Resource);
    }

    auto http = _make_http();
    if (!http) {
        return failed_result("HTTP client unavailable", FetchFailureKind::Resource);
    }
    HttpCloseGuard close_guard(http.get());

    http->SetTimeout(static_cast<int>(kHttpTimeoutMs));
    http->SetKeepAlive(false);
    http->SetHeader("Accept", "application/json");
    http->SetHeader("User-Agent", "StackChan-SL-Bus");

    if (_cancelled && _cancelled()) {
        return cancelled_result();
    }
    if (!http->Open("GET", std::string(stop.url))) {
        return failed_result("Could not connect to SL", FetchFailureKind::Transport);
    }

    const int status_code = http->GetStatusCode();
    const uint32_t retry_after = parse_retry_after_seconds(http->GetResponseHeader("Retry-After"));
    if (status_code <= 0) {
        return failed_result("SL request timed out", FetchFailureKind::Transport, 0, retry_after);
    }
    if (status_code != 200) {
        return failed_result("SL request failed", FetchFailureKind::HttpStatus, status_code, retry_after);
    }

    const size_t advertised_length = http->GetBodyLength();
    if (advertised_length > kMaxResponseBytes) {
        return failed_result("SL response is too large", FetchFailureKind::InvalidResponse, status_code,
                             retry_after);
    }

    std::string response;
    response.reserve(std::min(advertised_length, kMaxResponseBytes));
    std::array<char, 1024> buffer{};
    while (true) {
        if (_cancelled && _cancelled()) {
            close_guard.close();
            return cancelled_result();
        }

        const int received = http->Read(buffer.data(), buffer.size());
        if (received < 0) {
            return failed_result("Could not read SL response", FetchFailureKind::Transport, status_code,
                                 retry_after);
        }
        if (received == 0) {
            break;
        }
        const auto received_size = static_cast<size_t>(received);
        if (received_size > kMaxResponseBytes - response.size()) {
            return failed_result("SL response is too large", FetchFailureKind::InvalidResponse, status_code,
                                 retry_after);
        }
        response.append(buffer.data(), received_size);
    }
    close_guard.close();

    if (_cancelled && _cancelled()) {
        return cancelled_result();
    }

    const auto parsed = parse_departures(response, stop.line, stop.destination);
    if (!parsed.valid) {
        return failed_result(parsed.error.empty() ? "Malformed SL response" : parsed.error,
                             FetchFailureKind::InvalidResponse, status_code, retry_after);
    }

    FetchResult result;
    result.status = FetchResultStatus::Success;
    result.data = parsed.data;
    return result;
}

FetchResult DepartureClient::fetch_with_retry(const StopConfig& stop, const RetryWait& retry_wait) const
{
    auto result = fetch(stop);
    if (result.status != FetchResultStatus::Failed || result.failure_kind != FetchFailureKind::Transport ||
        result.retry_after_seconds > 0) {
        return result;
    }

    if (_cancelled && _cancelled()) {
        return cancelled_result();
    }
    if (retry_wait && retry_wait(250)) {
        return cancelled_result();
    }
    if (_cancelled && _cancelled()) {
        return cancelled_result();
    }
    return fetch(stop);
}

}  // namespace sl_bus
