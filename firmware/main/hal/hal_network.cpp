/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <stackchan/stackchan.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <wifi_manager.h>
#include <board.h>
#include <mutex>
#include <queue>
#include <vector>
#include <ctime>
#include <sys/time.h>
#include <esp_sntp.h>
#include <freertos/task.h>
#include <atomic>
#include <memory>

namespace {

struct CancellableNetworkContext {
    std::atomic<bool> connected{false};
    std::atomic<bool> active{true};
    std::function<void(std::string_view)> on_log;
};

}  // namespace

static std::string _tag           = "Network";
static bool _is_network_connected = false;

static void time_sync_notification_cb(struct timeval* tv)
{
    mclog::tagInfo(_tag, "SNTP time synchronized");
    GetHAL().syncSystemTimeToRtc();
}

void Hal::startSntp()
{
    mclog::tagInfo(_tag, "SNTP init");

    if (esp_sntp_enabled()) {
    } else {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);

        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_setservername(1, "time.google.com");
        esp_sntp_setservername(2, "cn.pool.ntp.org");

        sntp_set_time_sync_notification_cb(time_sync_notification_cb);

        esp_sntp_init();
    }
}

void Hal::startNetwork(std::function<void(std::string_view)> onLog)
{
    if (_is_network_connected) {
        mclog::tagInfo(_tag, "network already connected");
        return;
    }

    std::atomic<bool> network_connected = false;

    auto& board = Board::GetInstance();
    mclog::tagInfo(_tag, "start and wait for network connected...");

    board.SetNetworkEventCallback([&network_connected, &onLog](NetworkEvent event, const std::string& data) {
        switch (event) {
            case NetworkEvent::Scanning:
                if (onLog) {
                    onLog("WiFi scanning...");
                }
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    if (onLog) {
                        onLog("WiFi connecting...");
                    }
                } else {
                    if (onLog) {
                        onLog(fmt::format("Connecting to {} ...", data));
                    }
                }
                break;
            }
            case NetworkEvent::Connected: {
                network_connected = true;
                break;
            }
            case NetworkEvent::Disconnected:
                break;
            case NetworkEvent::WifiConfigModeEnter: {
                auto& wifi_manager = WifiManager::GetInstance();
                auto msg = fmt::format("Enter WiFi config mode. Hotspot: {}, Config URL: {}", wifi_manager.GetApSsid(),
                                       wifi_manager.GetApWebUrl());
                if (onLog) {
                    onLog(msg);
                }
                break;
            }
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                break;
            case NetworkEvent::ModemErrorNoSim:
                break;
            case NetworkEvent::ModemErrorRegDenied:
                break;
            case NetworkEvent::ModemErrorInitFailed:
                break;
            case NetworkEvent::ModemErrorTimeout:
                break;
        }
    });
    board.StartNetwork();

    while (!network_connected) {
        GetHAL().delay(500);
    }
    mclog::tagInfo(_tag, "network connected");
    board.SetNetworkEventCallback(nullptr);

    startSntp();

    _is_network_connected = true;
}

bool Hal::startNetworkCancellable(std::function<bool()> isCancelled,
                                  std::function<void(std::string_view)> onLog)
{
    auto& wifi = WifiManager::GetInstance();
    if (wifi.IsConnected()) {
        _is_network_connected = true;
        return true;
    }

    _is_network_connected = false;
    auto context = std::make_shared<CancellableNetworkContext>();
    context->on_log = std::move(onLog);

    auto& board = Board::GetInstance();
    board.SetNetworkEventCallback([context](NetworkEvent event, const std::string&) {
        if (!context->active.load()) {
            return;
        }
        switch (event) {
            case NetworkEvent::Scanning:
                if (context->on_log) context->on_log("WiFi scanning...");
                break;
            case NetworkEvent::Connecting:
                if (context->on_log) {
                    context->on_log("WiFi connecting...");
                }
                break;
            case NetworkEvent::Connected:
                context->connected.store(true);
                break;
            case NetworkEvent::Disconnected:
            case NetworkEvent::WifiConfigModeEnter:
            case NetworkEvent::WifiConfigModeExit:
            case NetworkEvent::ModemDetecting:
            case NetworkEvent::ModemErrorNoSim:
            case NetworkEvent::ModemErrorRegDenied:
            case NetworkEvent::ModemErrorInitFailed:
            case NetworkEvent::ModemErrorTimeout:
                break;
        }
    });

    if (isCancelled && isCancelled()) {
        context->active.store(false);
        board.SetNetworkEventCallback(nullptr);
        return false;
    }

    board.StartNetwork();
    while (!context->connected.load() && !wifi.IsConnected() && !(isCancelled && isCancelled())) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    const bool cancelled = isCancelled && isCancelled();
    context->active.store(false);
    board.SetNetworkEventCallback(nullptr);
    if (cancelled) {
        return false;
    }
    if (!context->connected.load() && !wifi.IsConnected()) {
        return false;
    }

    _is_network_connected = true;
    startSntp();
    return true;
}

WifiStatus Hal::getWifiStatus()
{
    auto& wifi = WifiManager::GetInstance();

    if (wifi.IsConfigMode()) {
        return WifiStatus::None;
    }
    if (!wifi.IsConnected()) {
        return WifiStatus::None;
    }

    int rssi = wifi.GetRssi();
    if (rssi >= -65) {
        return WifiStatus::High;
    } else if (rssi >= -75) {
        return WifiStatus::Medium;
    }
    return WifiStatus::Low;
}
