/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "sl_bus_departures.h"

#include <functional>
#include <string>

namespace sl_bus {

enum class FetchResultStatus {
    Success,
    Failed,
    Cancelled,
};

struct FetchResult {
    FetchResultStatus status = FetchResultStatus::Failed;
    StopDepartureData data;
    std::string error;
    uint32_t retry_after_seconds = 0;
};

class DepartureClient {
public:
    using CancelPredicate = std::function<bool()>;

    explicit DepartureClient(CancelPredicate cancelled);

    FetchResult fetch(const StopConfig& stop) const;

private:
    CancelPredicate _cancelled;
};

}  // namespace sl_bus
