/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "sl_bus_departure_client.h"

#include <board.h>
#include <hal/hal.h>
#include <http.h>
#include <network_interface.h>

#include <array>
#include <algorithm>
#include <memory>

namespace sl_bus {
namespace {

FetchResult failed_result(std::string_view error, uint32_t retry_after = 0)
{
    FetchResult result;
    result.status = FetchResultStatus::Failed;
    result.error = bounded_text(error);
    result.retry_after_seconds = retry_after;
    return result;
}

FetchResult cancelled_result()
{
    FetchResult result;
    result.status = FetchResultStatus::Cancelled;
    return result;
}

}  // namespace

DepartureClient::DepartureClient(CancelPredicate cancelled) : _cancelled(std::move(cancelled)) {}

FetchResult DepartureClient::fetch(const StopConfig& stop) const
{
    if (_cancelled && _cancelled()) {
        return cancelled_result();
    }

    auto* network = Board::GetInstance().GetNetwork();
    if (!network) {
        return failed_result("Network unavailable");
    }

    auto http = network->CreateHttp(0);
    if (!http) {
        return failed_result("HTTP client unavailable");
    }

    http->SetTimeout(static_cast<int>(kHttpTimeoutMs));
    http->SetKeepAlive(false);
    http->SetHeader("Accept", "application/json");
    http->SetHeader("User-Agent", "StackChan-SL-Bus");

    if (_cancelled && _cancelled()) {
        return cancelled_result();
    }
    if (!http->Open("GET", std::string(stop.url))) {
        http->Close();
        return failed_result("Could not connect to SL");
    }

    const int status_code = http->GetStatusCode();
    const uint32_t retry_after = parse_retry_after_seconds(http->GetResponseHeader("Retry-After"));
    if (status_code != 200) {
        http->Close();
        if (status_code < 0) {
            return failed_result("SL request timed out", retry_after);
        }
        return failed_result("SL request failed", retry_after);
    }

    const size_t advertised_length = http->GetBodyLength();
    if (advertised_length > kMaxResponseBytes) {
        http->Close();
        return failed_result("SL response is too large", retry_after);
    }

    std::string response;
    response.reserve(std::min(advertised_length, kMaxResponseBytes));
    std::array<char, 1024> buffer{};
    while (true) {
        if (_cancelled && _cancelled()) {
            http->Close();
            return cancelled_result();
        }

        const int received = http->Read(buffer.data(), buffer.size());
        if (received < 0) {
            http->Close();
            return failed_result("Could not read SL response", retry_after);
        }
        if (received == 0) {
            break;
        }
        if (response.size() + static_cast<size_t>(received) > kMaxResponseBytes) {
            http->Close();
            return failed_result("SL response is too large", retry_after);
        }
        response.append(buffer.data(), static_cast<size_t>(received));
    }
    http->Close();

    if (_cancelled && _cancelled()) {
        return cancelled_result();
    }

    const auto parsed = parse_departures(response, stop.line, stop.direction_code);
    if (!parsed.valid) {
        return failed_result(parsed.error.empty() ? "Malformed SL response" : parsed.error, retry_after);
    }

    FetchResult result;
    result.status = FetchResultStatus::Success;
    result.data = parsed.data;
    return result;
}

}  // namespace sl_bus
