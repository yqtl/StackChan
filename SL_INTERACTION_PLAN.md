# SL interaction plan

Status: implementation handoff for Luna. This document defines the work; no
firmware has been changed or flashed as part of preparing it. Source reviewed
at `0661b62` (`Add SL bus departures app`), 2026-09-16.

## Start here — implementation contract

Implement **Phase 1**, including the host checks below, using the existing
`GESTURE` and `SL.BUS` apps. Run Phase 0 hardware checks when a device is
available; missing hardware does not prevent implementing and testing the
transition. Report hardware checks as pending until actually performed.
Phases 2–3 are deferred experiments, not part of the first implementation.

The user-visible result is:

```text
Launcher -> user opens GESTURE -> confirmed thumbs-up
         -> finish Gesture worker and teardown -> SL.BUS
         -> Home -> Launcher
```

- Startup, launcher tiles, factory apps, bus routes and refresh policy stay as
  they are. Gesture does not run in the background or open automatically.
- One Gesture app opening can request SL.BUS at most once. A later manual
  reopening starts a fresh session. A Home action processed before dispatch
  cancels that session's automatic transition, including while stopping.
- Keep the manual GESTURE tile and preview/status diagnostics. An unwired
  confirmation callback must retain the current recognition-only behavior,
  so tests or diagnostic builds can run it without navigation. No new settings
  screen or duplicate diagnostic app is needed.
- Preserve `firmware/partitions.csv`, dependency pins, models, thresholds and
  selective optimization flags. Do not introduce a new library for this work.
- Deliver source, tests and recorded validation results. Building and host
  testing do not require a connected device. This document is not an instruction
  to flash; use the device checks below when a hardware update is in scope.

All shell commands must use `rtk` (use `rtk proxy` for unwrapped commands).
Read the current worktree before editing and preserve unrelated user changes.

## Recommendation

Use the existing `SL.BUS` app as the dependable base, connect the working
thumbs-up prototype to it as the first interactive demo, and evaluate automatic
presence detection as a separate hardware spike.

The first useful milestone is:

```text
GESTURE -> confirmed thumbs-up -> SL.BUS -> Home returns to Launcher
```

Do not make automatic opening depend on the gesture model. For a person at
1–2 metres, test camera face detection before committing to it. The built-in
LTR-553 proximity sensor should be treated as a close-range trigger until its
actual range on this enclosure is measured.

## Current baseline and constraints

- `SL.BUS` source already fetches the configured SL departures for bus 57 at
  Tullgårdsparken and bus 74 at Bohusgatan, refreshes them in a worker, and
  handles stale or unavailable data.
- `GESTURE` runs hand detection and classification locally. The latest
  recorded device samples take about 891 ms for a classified frame and require
  two positive observations, so a deliberate hold of roughly 1–2 seconds is
  expected.
- The local `firmware/build/stack-chan.bin` measured on 2026-09-16 is
  5,093,856 bytes in a 5,177,344-byte (`0x4f0000`) OTA slot, leaving only
  83,488 bytes. This is a local artifact, not proof of a fresh build or of the
  installed version. Remeasure after building; never reuse this as a pass result.
- The CoreS3 has an ESP32-S3 at 240 MHz, 16 MB flash, 8 MB PSRAM, a 0.3 MP
  camera, and an LTR-553 proximity/ambient-light sensor. Camera inference and
  HTTPS must not run concurrently without measuring internal-heap headroom.
- Preserve the existing partition table, gesture model partitions, and factory
  `ota_1` rollback image. No new model partition is assumed.

## Source map and lifecycle facts

Paths below are relative to the repository root. Read these before coding.

| File | Relevant code / intended change |
| --- | --- |
| `firmware/main/main.cpp` | `app_main()` installs apps. Retain typed pointers before moving unique pointers into Mooncake, and retain returned Launcher, Gesture and SL.BUS IDs. Wire the callback after installation, before the update loop. |
| `firmware/main/apps/app_gesture/app_gesture.{h,cpp}` | `recognize()`, `report()`, `onRunning()`, `stopWorker()`, `onClose()`: publish confirmation separately from status; add shutdown state and UI-thread dispatch. |
| `firmware/main/apps/app_gesture/gesture_gate.hpp` | Existing recognition gate; keep semantics and thresholds unchanged. |
| `firmware/main/apps/app_launcher/app_launcher.{h,cpp}` | Add deferred transition request and override `onSleeping()` to wait for source teardown before calling the base launcher. |
| `firmware/components/mooncake/src/templates/launcher.h` | Read `openApp()` and `onSleeping()`; do not edit this fetched dependency. |
| `firmware/components/mooncake/src/ability/app_ability.cpp` | `close()` only sets `StateGoClose`; `baseUpdate()` calls `onClose()` later, then sets `StateSleeping`. |
| `firmware/main/apps/app_sl_bus/app_sl_bus.cpp` | `onOpen()` starts its worker immediately. Keep client and polling behavior intact; add lifecycle diagnostics only if needed. |
| `firmware/main/apps/app_sl_bus/sl_bus_departures.{h,cpp}` | Existing routes, snapshots, freshness and retry policy. |
| `firmware/main/hal/board/stackchan_camera.{h,cc}` | Shared board-owned camera; `StreamCaptures(true)` returns driver buffers and retains a reusable frame copy. Do not delete the camera or its borrowed frame data. |
| `firmware/tests/CMakeLists.txt` | Register new host transition tests alongside existing tests. |

Two traps must be addressed explicitly:

1. `_results` is a length-one overwrite queue. `Result::confirmed` represents
   the current display state while the recognition gate is latched; it is not
   a reliable one-shot event. A later status can overwrite it, and many frames
   can carry `confirmed=true`.
2. The base launcher's `openApp(id)` only queues an ID. Its next `onSleeping()`
   opens that ID without checking whether the previous app finished closing.
   Launcher is installed before Gesture, so it can run before Gesture's next
   `onClose()`. Simply calling `launcher.openApp(slId); gesture.close();` can
   start networking while inference resources and shared Home/status UI remain.
   Calling `GetMooncake().openApp(slId)` directly also bypasses launcher's
   `_running_app_id` bookkeeping and breaks the intended Home route.

## Implementation phases

### Phase 0 — Validate the existing SL app (0.5–1 engineering day)

Run the current `SL.BUS` app on the device with working Wi-Fi and offline or
failed-network conditions. Verify the two routes, Swedish characters,
30-second refresh, stale-data state, Home navigation, and repeated open/close.
Record the application size and free internal heap after opening and closing.
Use `sl_bus::kStops` as the source configuration: site 1314 / line 57 /
direction code 2 and site 1318 / line 74 / direction code 2. This phase validates
the existing configuration; it does not change destinations based on this
document. Check immediate fetch, 30-second successful refresh, stale marking
after failure, unavailable values after 90 seconds without success, and recovery.

Record baseline issues separately. Fix only issues that prevent this interaction
from working; do not turn Phase 1 into a bus UI redesign. Phase 0 timing estimates
are hands-on effort and exclude the 24-hour soak required for a reliability claim.

### Phase 1 — Connect thumbs-up to SL.BUS (1–2 engineering days)

Implement in this order. API names below are proposed names for new code;
they do not already exist.

#### 1. Publish a reliable confirmation

- Add `std::atomic<bool> _thumbs_up_pending{false}` to `AppGesture` and a
  UI-thread callback `std::function<bool()> onThumbsUpConfirmed`. The callback
  returns whether Launcher accepted a deferred transition; it performs no
  network work. Configure it only while the app is not running.
- Store `true` only at the existing `if (gate.update(...))` confirmation edge
  beside `THUMBS_UP_CONFIRMED`. The worker never calls the callback or Mooncake.
  Leave the status queue as presentation only.
- Consume with `exchange(false)` on the Mooncake thread. Reset pending and
  per-session navigation state before starting a new worker, and clear pending
  again after joining on close. Never clear/reset into a new session while an
  old worker can still publish. An event with no callback is consumed without
  stopping recognition, preserving diagnostic behavior.
- Keep the existing `like` label, 0.85 classifier threshold, single-hand rule,
  250 ms / two-observation hold, 1000 ms release and 1200 ms maximum gap.
  A confirmation is not a 250 ms end-to-end response guarantee.

#### 2. Drain Gesture without blocking the normal UI loop

Use a small UI-owned state (`Recognizing`, `StoppingForSl`, `StoppingForHome`,
`CloseRequested`) and a per-session dispatch latch. Keep this decision logic
in a host-testable helper under `app_gesture/`; the actual worker still uses
the existing atomic stop flag and completion semaphore.

| Input/state | Required action |
| --- | --- |
| Confirmation in Recognizing, callback wired | Latch intent, set `_stop=true`, enter StoppingForSl, show `Opening SL.BUS...`. |
| Home in Recognizing or StoppingForSl | Enter StoppingForHome, clear pending confirmation, set `_stop=true`; Home takes priority. |
| Any duplicate confirmation while stopping/closing | Ignore. |
| Worker still active | Update Home/status UI and return from `onRunning()`; poll completion with zero timeout. |
| Worker done in StoppingForHome | Request `close()` once, without invoking the callback. |
| Worker done in StoppingForSl | Invoke callback once outside the LVGL lock, then request `close()` once. Rejection logs a warning and returns to Launcher, with no retry. |

Process `view::update_home_indicator()` before consuming confirmation or
dispatching in the same tick. Change Gesture's Home callback from immediate
`close()` to the stop-intent path above. Continue updating the UI while draining;
do not wait on `_done` while holding `LvglLockGuard` because the worker itself
uses that lock. Once a transition has been dispatched and `close()` requested,
the old app stops accepting UI input.

Refactor completion handling so a successful zero-timeout take marks
`_worker_started=false`; `stopWorker()` must not take the same binary semaphore
again. No worker started (allocation/task-creation failure) counts as already
drained. On normal exit or `std::bad_alloc`, `recognize()`'s local models, RGB
buffer and converter must be destroyed before `_done` is signaled. Retain the
rule that the worker never touches app members after signaling completion.

`onClose()` remains the single UI-resource cleanup path. Keep `stopWorker()`
as the join fallback for destruction or externally forced close; its wait must
remain outside the LVGL lock. Normal Home and gesture paths join asynchronously
first, so their `onClose()` has no inference wait. Delete preview/status/Home
objects and free `_preview_pixels` only after the worker has finished.

Here “release the camera” means Gesture has stopped accessing the shared camera
and has freed its own resources. The board retains its camera and frame copy;
do not deinitialize/free those to satisfy a heap check. Never force-delete an
inference task or start SL after an arbitrary shutdown timeout. If draining
takes over five seconds, log once and show a stopping message, keep servicing
the UI/watchdog, and flag hardware validation as failed until investigated.

#### 3. Defer launching until Gesture is Sleeping

Add `bool AppLauncher::requestAppAfterClose(int sourceId, int targetId)` and
an `onSleeping()` override (not just `onLauncherSleeping()`, which runs too late
inside the base implementation). Store at most one source/target pair.

The request is called only from the Mooncake thread. Accept only if:

- Launcher is `StateSleeping` and `getRunningAppId() == sourceId`;
- source and target exist, are distinct, and neither is Launcher;
- source is `StateRunning`, target is `StateSleeping`, and neither a deferred
  request nor the base `_going_to_open_app_id` is already pending.

On rejection return false without changing launcher state. In `onSleeping()`:

1. With no deferred request, call `AppLauncherBase::onSleeping()` normally.
2. With a valid pending source still Running/GoClose, return without calling
   the base. Do not queue the target yet. This keeps the existing running ID
   until cleanup finishes.
3. When the source becomes `StateSleeping`, revalidate target and running ID,
   call inherited `openApp(targetId)`, clear the deferred pair, and call the
   base `onSleeping()`. The base opens the target and updates its running ID.
4. If the source disappears/reopens unexpectedly, the running ID changes, or
   the target becomes invalid/non-sleeping, discard the pair and log the reason.
   Let normal base handling resume. If the tracked source was uninstalled,
   explicitly clear that obsolete running ID and reopen Launcher; the base
   checks Sleeping only and cannot recover from `StateNull` by itself.

Keep this deferred-launch logic in a small reusable header under
`app_launcher/` (e.g. `deferred_app_launcher.hpp`, deriving from
`AppLauncherBase`), and derive `AppLauncher` from it. This lets host tests use
the production implementation with fake apps and real Mooncake lifecycle code,
without linking the hardware launcher view. Do not patch the fetched Mooncake
component or duplicate its whole lifecycle in the application.

In `main.cpp`, save raw typed pointers from Launcher and Gesture unique pointers
before moving them to `installApp`, and store all three returned IDs. After all
installs, wire Gesture's callback to
`launcher->requestAppAfterClose(gestureId, slBusId)` if IDs are valid. Do not
hard-code numeric IDs or resolve names every frame. The objects stay owned by
Mooncake; callbacks must not execute during `uninstallAllApps()`/destructors.
The existing `skip_mooncake` / AI startup branch remains unchanged.

Expected ordering, regardless of app iteration order:

```text
worker: gate edge -> atomic pending
Gesture onRunning: Home first -> stop intent -> poll done over later ticks
worker: unwind local resources -> signal done -> no more app access
Gesture onRunning: observe done -> requestAppAfterClose -> close()
Gesture onClose: delete UI/free preview -> return
Mooncake: Gesture StateSleeping
Launcher onSleeping: inherited openApp(SL) -> base updates running ID
SL onOpen: create UI and start network worker
```

#### 4. Keep the integration narrow and observable

Reuse `AppSlBus` and its network worker unchanged. Confirmation selects the
app; it never calls `DepartureClient::fetch()` itself. Add concise transition
logs for confirmation consumption, worker drain completion, Gesture close
completion, launch accepted/rejected and SL open. Include monotonic timestamps
and internal free heap / largest free internal block at lifecycle boundaries,
not on every UI tick. These logs must prove cleanup precedes SL worker startup.
Keep application code in tracked files; generated/dependency-tree edits will
not survive a clean clone.

Acceptance criteria:

- A single confirmation opens `SL.BUS` once; the worker cannot open it twice
  from repeated frames.
- No hand, two hands, another tested gesture, or a cancelled/stalled
  confirmation opens it.
- Normal Home/gesture closing during capture or classification keeps the UI
  loop responsive while draining and leaves the camera usable by the next app.
- Home returns to Launcher; reopening either app works repeatedly.
- The image still fits the unchanged OTA slot and the factory partition ranges
  are untouched.

### Phase 2 — Presence feasibility spike (2–4 engineering days)

Deferred until the manual demo is delivered and presence work is requested.
Run this without changing the default launcher behavior. Save results in a
separate feasibility report with model/version, build size, raw readings,
latency and heap measurements; this phase does not enable automatic launching.

**Proximity path:** add a small test harness for the LTR-553 on the existing
I2C bus. Log the raw value and calibrated near/far state at several distances.
Use hysteresis and a short debounce in the experiment. Treat a positive result
as “someone or something is very close”; do not infer 1–2 metre capability
from the sensor specification.

**Camera path:** build a separate test configuration using an ESP-DL human-face
detector at a low sample rate (about 1–2 frames/second). First measure model
size, heap use, and latency; do not add it to the shipping partition table
until those checks pass. Require a face to be detected in consecutive samples,
and log distance, lighting, angle, false triggers, and time to detection. Do
not identify or store people.

Use ten trials at each of 0.5 m, 1 m, and 2 m in the intended lighting. A
candidate automatic trigger must detect at least 9/10 approaches within three
seconds and produce no more than one false activation during a 30-minute idle
run. If it misses either gate, keep presence out of the product path.

### Phase 3 — Automatic opening, only if Phase 2 passes (3–5 engineering days)

1. Add a `PresenceDetector` interface with separate LTR-553 and camera-backed
   experiments; integrate only the implementation supported by Phase 2 evidence.
   The launcher owns the monitor and stops/joins it before any foreground app
   opens. `SL.BUS` does not poll the camera; `GESTURE` retains its own manual
   camera worker, mutually exclusive with the launcher monitor.
2. While Launcher is idle, require the validated presence condition and a
   60-second cooldown before requesting `SL.BUS`. Route the request through the
   launcher so normal lifecycle and Home behavior remain intact.
3. Keep the first automatic version open until the user presses Home. Do not
   add an automatic timeout until real use shows that it is needed.
4. If the face model exceeds the application slot or model storage, stop at the
   feasibility result. Options then are a smaller model, SD-card loading, or an
   external presence sensor; do not resize or move factory partitions for this
   feature.

## Verification and handoff

### Host tests required for Phase 1

Add `gesture_transition_test.cpp` for the production UI-state helper and
`deferred_app_launch_test.cpp` for the production deferred launcher. Register
both with CTest. Use real Mooncake `src/mooncake.cpp`, `src/ability/*.cpp`, and
`src/ability_manager/*.cpp` for the latter with include path
`firmware/components/mooncake/src`; use fake Gesture/SL apps to record lifecycle
calls. No ESP-IDF/LVGL stubs should be needed for these isolated helpers.

Required assertions:

- A confirmation survives later status overwrites; repeated edges request at
  most one launch per opening. No callback keeps diagnostics running.
- Home and confirmation in the same tick choose Home; Home during drain also
  cancels. A late worker event during cancellation cannot revive the request.
- Waiting for worker completion does not dispatch or request `close()` early;
  already-finished/no-worker paths complete without waiting twice.
- Invalid IDs, duplicate requests, a different running app, a target already
  running and a missing target are rejected/cancelled without launching.
- Source Running and GoClose do not open SL. Source `onClose()` completion and
  Sleeping precede SL `onOpen()`. Exercise Launcher-before-source and
  source-before-Launcher installation orders with an explicitly opened launcher.
- Closing SL causes Launcher to reopen with its running ID cleared. Repeat ten
  Gesture -> SL -> Home cycles; also test manual Gesture -> Home and manual
  Launcher -> SL -> Home. Pending events/requests must not cross sessions.
- Source disappearance clears the request and recovers the launcher; a rejected
  callback closes Gesture to Launcher and is not retried.

Keep existing `gesture_gate_test`, `sl_bus_parser_test`, and `motion_math_test`
passing. Gate tests remain responsible for no-hand/multiple-hand input, rejected
poses, interruptions, duplicate timestamps and stalled confirmations; hardware
still needs to verify the model produces appropriate observations.

### Reproducible commands

Run from the repository root in an activated ESP-IDF **5.5.4** environment.
To check activation, substitute the actual installed path:

```bash
rtk proxy bash -c 'source /absolute/path/to/esp-idf/export.sh && idf.py --version'
rtk git status --short
```

Activation in that subprocess does not persist to subsequent tool calls. If
the runner lacks an activated environment, wrap each environment-dependent
command in `rtk proxy bash -c 'source /actual/path/export.sh && ...'`. The
placeholder is not a checkout path to copy literally. No machine-specific
toolchain path is required by this plan.

Build first on a clean clone: the helper fetches pinned repositories and
resolves managed dependencies, including cJSON needed by host tests. Network
access is required if they are absent. Existing pinned versions include
ESP-DL 3.3.2 and ESP-SR 2.4.5; retain `dependencies.lock` and `repos.json`.
Review local edits in fetched repos before running the helper, which fetches
and checks out their pinned refs. Do not discard such edits to make it run.

```bash
rtk proxy bash -c 'cd firmware && ./build_gesture.sh > /tmp/stackchan-interaction-build.log 2>&1'
rtk proxy tail -n 25 /tmp/stackchan-interaction-build.log
rtk proxy cmake -S firmware/tests -B /tmp/stackchan-interaction-tests -DCMAKE_CXX_FLAGS='-Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=undefined'
rtk proxy cmake --build /tmp/stackchan-interaction-tests
rtk proxy ctest --test-dir /tmp/stackchan-interaction-tests --output-on-failure
rtk proxy python3 firmware/tests/check_gesture_link_map.py firmware/build/stack-chan.map
rtk proxy python3 -m esptool image-info firmware/build/stack-chan.bin
rtk proxy stat -c '%n %s bytes' firmware/build/stack-chan.bin firmware/build/espdl_models/hand_detect.espdl firmware/build/espdl_models/hand_gesture_cls.espdl
rtk git diff --check
rtk git diff -- firmware/partitions.csv firmware/dependencies.lock firmware/repos.json
```

Use the installed esptool's `image_info` spelling if its CLI rejects
`image-info`; require both checksum and validation hash to pass. The build
helper already runs the link-map check; its explicit command is useful when
inspecting artifacts independently. Require all 12 hand convolution entry
points to resolve to ESP-DL. Check each measured size against its unchanged
partition: application <= `0x4f0000`, hand detector <= `0x80000`, classifier <=
`0xd0000`. Record application bytes and remaining headroom. Do not use an old
artifact after a failed build. The final diff of the three protected files
above should be empty relative to the starting worktree.

### Hardware checklist (report pending if unavailable)

Before a device update, verify the connected chip/flash, device partition table,
current boot selection and generated `firmware/build/flasher_args.json`. Require
the intended custom application slot to be `ota_0` at `0x20000`, with `ota_1`
at `0x510000` preserved as factory rollback. If device state differs, report it
and resolve the actual update target before writing. The generated full flash
plan includes other regions; do not invoke `idf.py flash` blindly. An app-only
update must touch only the verified app range, not bootloader, partition table,
OTA selection, assets, NVS, or models. Verify the written hash and boot version.

- Open SL manually online and offline; verify the Phase 0 behavior and Home
  during network connection, a slow request, and backoff.
- Open Gesture; check no hand, one deliberate thumbs-up, sustained thumbs-up,
  two hands, and other poses. Only a valid confirmation opens SL once.
- Press Home before confirmation and while stopping. Test closing during
  capture/classification and model loading. No cancelled request may open SL.
- Verify log order: Gesture drain -> Gesture close complete -> SL open/network
  start. No inference log may appear after drain until Gesture is reopened.
- Repeat ten full cycles, then reopen Gesture and another existing camera
  consumer. Compare internal free heap and largest internal block at the same
  settled Launcher state after each cycle; allow initial one-time caches, but
  investigate continuing declines, allocation failures or fragmentation.
- Scan serial logs for panic, assertion, watchdog and stack-canary markers.
  Measure confirmation-to-SL-display time; report it rather than treating the
  earlier 891 ms inference sample as transition latency.
- Confirm Swedish characters and readability at 1–2 metres. Run the existing
  bus plan's **24-hour powered soak** before claiming reliability; record
  resets, stale-data handling and heap trend. This is distinct from code/build
  completion and is not silently waived if hardware is unavailable.

### Completion report

Update this document with changed files, test counts/results, build version,
application size/headroom and paths to relevant logs. Mark each hardware item
passed, failed or pending. State any unavailable toolchain/dependency/device
explicitly while completing independent work. Phase 1 source completion means
the host suite, full firmware build, link-map guard and image checks pass;
hardware acceptance and reliability remain separate until demonstrated.
Do not implement presence or add model storage just to complete this report.

### Implementation report — 2026-09-16

Phase 1 source and host-test work is implemented. The full firmware build and
device validation remain pending because this environment has no activated
ESP-IDF installation (`idf.py`/`export.sh`) and no `esptool` Python module.

Changed application and test files:

- `firmware/main/apps/app_gesture/app_gesture.{h,cpp}` and
  `firmware/main/apps/app_gesture/gesture_transition.hpp`: separate atomic
  confirmation event, Home-priority nonblocking worker drain, completion-gated
  callback dispatch, cleanup diagnostics, and fresh-session reset.
- `firmware/main/apps/app_launcher/app_launcher.h` and
  `firmware/main/apps/app_launcher/deferred_app_launcher.hpp`: production
  deferred launch helper that waits for source `StateSleeping` and recovers
  after source disappearance.
- `firmware/main/main.cpp`: retained typed app pointers and returned IDs, then
  wired the Gesture-to-SL.BUS callback after installation.
- `firmware/main/apps/app_sl_bus/app_sl_bus.cpp`: lifecycle heap/timestamp log
  at SL.BUS open and network-worker startup.
- `firmware/tests/CMakeLists.txt`, `gesture_transition_test.cpp`, and
  `deferred_app_launch_test.cpp`: registered host coverage using real Mooncake
  lifecycle sources.

Validation recorded:

- CMake build completed all five host targets with `-Wall -Wextra -Werror`
  and UBSan. CTest passed 5/5: motion math, Gesture gate, Gesture transition,
  deferred launcher, and SL parser. The detailed result is at
  `/tmp/stackchan-interaction-tests/Testing/Temporary/LastTest.log`.
- `git diff --check` passed. `firmware/partitions.csv`,
  `firmware/dependencies.lock`, and `firmware/repos.json` remain unchanged.
- Configured project version is `1.5.1-gesture.7`. The existing local
  `firmware/build/stack-chan.bin` is 5,093,856 bytes against the 5,177,344-byte
  OTA slot, leaving 83,488 bytes, but it predates this implementation and is
  not a fresh build or acceptance result. Its pre-existing link-map guard
  reports 12 ESP-DL hand convolution kernels.

Hardware and firmware-image checks are pending: Phase 0 online/offline SL
validation, camera/gesture transition trials, ten-cycle heap/log checks,
partition/boot verification, and the 24-hour soak were not run. No firmware
was flashed.

## Effort summary

| Milestone | Engineering effort | Outcome |
| --- | ---: | --- |
| Existing SL validation | 0.5–1 day | Known-good display baseline |
| Gesture-triggered SL demo | 1–2 days | Reliable manual interaction |
| Presence feasibility | 2–4 days | Evidence for or against 1–2 m activation |
| Automatic presence integration | 3–5 days | Launcher-triggered SL display, only if feasible |

The recommended first demo is therefore about 1.5–3 engineering days. The
automatic approach is a follow-on experiment, not a prerequisite for delivering
useful SL information.

## Out of scope

Cloud image upload, face recognition or identity storage, continuous camera
streaming in `SL.BUS`, changes to factory partitions, and blind flashing of an
image that has not passed the size and partition checks.
