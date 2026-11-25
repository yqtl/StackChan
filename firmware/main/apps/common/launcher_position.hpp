#pragma once
#include <mooncake.h>

namespace common {

/**
 * @brief App ID of the launcher, recorded when it is created
 *
 */
inline int LauncherAppId = -1;

/**
 * @brief Position of an app's tile in the launcher, which lists every installed app except itself in install
 * order. Pass it to requestWarmReboot() so the launcher reopens on that tile, whichever factory apps are built.
 *
 * @param appId
 * @return int
 */
inline int get_launcher_position(int appId)
{
    int position = 0;
    for (const auto& props : mooncake::GetMooncake().getAllAppProps()) {
        if (props.appID == LauncherAppId) {
            continue;
        }
        if (props.appID == appId) {
            return position;
        }
        position++;
    }
    return 0;
}

}  // namespace common
