/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "sl_bus_departures.h"

#include <functional>
#include <memory>
#include <string>

#include <http.h>

namespace sl_bus {

enum class FetchResultStatus {
    Success,
    Failed,
    Cancelled,
};

enum class FetchFailureKind {
    None,
    Transport,
    HttpStatus,
    InvalidResponse,
    Resource,
};

struct FetchResult {
    FetchResultStatus status = FetchResultStatus::Failed;
    StopDepartureData data;
    std::string error;
    uint32_t retry_after_seconds = 0;
    FetchFailureKind failure_kind = FetchFailureKind::None;
    int http_status = 0;
};

class DepartureClient {
public:
    using CancelPredicate = std::function<bool()>;
    using HttpFactory = std::function<std::unique_ptr<Http>()>;
    using RetryWait = std::function<bool(uint32_t delay_ms)>;

    DepartureClient(CancelPredicate cancelled, HttpFactory make_http);

    FetchResult fetch(const StopConfig& stop) const;
    FetchResult fetch_with_retry(const StopConfig& stop, const RetryWait& retry_wait) const;

private:
    CancelPredicate _cancelled;
    HttpFactory _make_http;
};

// The board-dependent implementation lives outside the client core so host
// tests can inject a scripted Http without pulling in ESP-IDF or board code.
DepartureClient::HttpFactory make_board_http_factory();

}  // namespace sl_bus
