/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "sl_bus_departure_client.h"

#include <board.h>
#include <network_interface.h>

namespace sl_bus {

DepartureClient::HttpFactory make_board_http_factory()
{
    return []() {
        auto* network = Board::GetInstance().GetNetwork();
        if (!network) {
            return std::unique_ptr<Http>{};
        }
        return network->CreateHttp(0);
    };
}

}  // namespace sl_bus
