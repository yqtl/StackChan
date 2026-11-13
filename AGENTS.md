# StackChan (fork)

Fork of M5Stack StackChan: ESP-IDF 5.5.4 firmware for the CoreS3-based robot, target `esp32s3` (`firmware/`); Flutter app (`app/`); Go server (`server/`); remote-control firmware, a separate ESP-IDF 5.4.2 project for `esp32` (`remote/code/`); LAN surveillance receiver and provisioning tools (`tools/surveillance/`).

## Build and test
- `idf.py`, `cmake` and `ctest` come from the ESP-IDF environment, which a new shell does not have. Source ESP-IDF 5.5.4's `export.sh` first; the "Build and install on StackChan" section of `tools/surveillance/README.md` has the `IDF_TOOLS_PATH` and `export.sh` lines for this machine.
- Firmware, from `firmware/`: `python3 ./fetch_repos.py && idf.py build`. `fetch_repos.py` needs the network: it clones or fetches the repos in `repos.json` into `components/` and `xiaozhi-esp32/` and applies `patches/xiaozhi-esp32.patch` (skipped when already applied). Those trees and `managed_components/` are git-ignored; when they are already present and `repos.json` has not changed, `idf.py build` alone is enough.
- Gesture build: `firmware/build_gesture.sh` runs `fetch_repos.py` and `idf.py reconfigure`, applies `patches/esp-dl-3.3.2-cmake.patch` to `managed_components/espressif__esp-dl`, runs `idf.py build`, then checks the link map with `tests/check_gesture_link_map.py`.
- Host tests (no device), from `firmware/`: `cmake -S tests -B build-host-tests && cmake --build build-host-tests && ctest --test-dir build-host-tests --output-on-failure`.
- Host tests compile sources from `components/mooncake` and `managed_components/`. Each test is listed by hand in `firmware/tests/CMakeLists.txt`; a new test file is not built until it is added there.
- `firmware/build-host-tests/` is not git-ignored; do not commit it.
- Receiver and provisioning tests, from the repo root: `python3 -m unittest discover -s tools/surveillance -p 'test_*.py' -v` (temp directories only, no third-party packages).
- Keep build output in this checkout's `firmware/build` (git-ignored).
- Apps are declared in `firmware/main/apps/apps.h` and installed in `firmware/main/main.cpp` with `GetMooncake().installApp(...)`. `firmware/main/CMakeLists.txt` globs `apps/*.cpp` at configure time, so run `idf.py reconfigure` after adding a source file.

## Device access
- Flash or open the serial monitor only when the user asks for it.
- Hold the exclusive lock shared by all sessions for the whole operation: `flock /tmp/stackchan-device.lock idf.py flash` (same for `idf.py monitor` and `esptool`). Builds and host tests need no lock.
- `tools/surveillance/README.md` documents an app-only flash for the surveillance firmware: check the board's partition table, then `esptool write_flash` of `firmware/build/stack-chan.bin` at `0x20000`. A plain `idf.py flash` also writes the bootloader, partition table, otadata, assets and gesture-model partitions (`flasher_args.json` in `firmware/build`).

## Secrets
- `tools/surveillance/provision.py` writes the git-ignored `firmware/main/apps/app_surveillance/connection_profile.h`, which holds the pairing token and the receiver CA certificate, and keeps the token, `receiver.json` and private keys in `~/.config/stackchan-surveillance/`. Never commit, print or paste their contents.
- Without the profile the firmware still builds but the Surveillance app cannot arm. Firmware built after provisioning embeds the token, so `firmware/build/` output contains it too.

## Hardware limits
- Pitch servo: `pitch_servo_config.angleLimit` in `Hal::servo_init()` (`firmware/main/hal/hal_servo.cpp`) stays `Vector2i(50, 850)`, which is 5.0 to 85.0 degrees in units of 0.1 degree (upstream used `Vector2i(30, 870)`). M5Stack's StackChan documentation recommends keeping the Y-axis servo within 5 to 85 degrees; extreme angles easily stall and damage it. Do not widen it.
- `Servo::update_angle_anim_target` (`firmware/main/stackchan/motion/servo.cpp`) clamps every target passed to `Servo::move`, `moveWithSpringParams` and `moveWithSpeed` to this range, so `Motion::goHome` puts the pitch at 5.0 degrees.
