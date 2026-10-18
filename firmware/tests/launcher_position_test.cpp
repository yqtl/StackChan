#include "launcher_position.hpp"

#include <cassert>
#include <iostream>
#include <memory>

namespace {

class NamedApp : public mooncake::AppAbility {
public:
    explicit NamedApp(const char *name)
    {
        setAppInfo().name = name;
    }
};

int install(const char *name)
{
    return mooncake::GetMooncake().installApp(std::make_unique<NamedApp>(name));
}

void full_factory_build_keeps_the_old_positions()
{
    common::LauncherAppId = install("Launcher");
    const int ai_agent    = install("AI.AGENT");
    const int avatar      = install("AVATAR");
    const int espnow      = install("ESPNOW.REMOTE");
    const int app_center  = install("APP.CENTER");
    const int ezdata      = install("EZDATA");
    const int dance       = install("DANCE");
    const int setup       = install("SETUP");
    const int gesture     = install("GESTURE");

    // These are the positions the factory apps used to pass to requestWarmReboot().
    assert(common::get_launcher_position(ai_agent) == 0);
    assert(common::get_launcher_position(avatar) == 1);
    assert(common::get_launcher_position(espnow) == 2);
    assert(common::get_launcher_position(app_center) == 3);
    assert(common::get_launcher_position(ezdata) == 4);
    assert(common::get_launcher_position(dance) == 5);
    assert(common::get_launcher_position(setup) == 6);
    assert(common::get_launcher_position(gesture) == 7);

    // Unchanged once Mooncake has created the apps.
    mooncake::GetMooncake().update();
    assert(common::get_launcher_position(setup) == 6);

    // An unknown app falls back to the first tile.
    assert(common::get_launcher_position(-1) == 0);

    mooncake::DestroyMooncake();
}

void lean_build_moves_setup_up()
{
    common::LauncherAppId = install("Launcher");
    const int ai_agent    = install("AI.AGENT");
    const int setup       = install("SETUP");
    const int gesture     = install("GESTURE");

    assert(common::get_launcher_position(ai_agent) == 0);
    assert(common::get_launcher_position(setup) == 1);
    assert(common::get_launcher_position(gesture) == 2);

    mooncake::DestroyMooncake();
}

void launcher_is_skipped_wherever_it_was_installed()
{
    const int ai_agent    = install("AI.AGENT");
    common::LauncherAppId = install("Launcher");
    const int setup       = install("SETUP");

    assert(common::get_launcher_position(ai_agent) == 0);
    assert(common::get_launcher_position(setup) == 1);

    mooncake::DestroyMooncake();
}

}  // namespace

int main()
{
    full_factory_build_keeps_the_old_positions();
    lean_build_moves_setup_up();
    launcher_is_skipped_wherever_it_was_installed();

    std::cout << "launcher_position_test passed\n";
    return 0;
}
