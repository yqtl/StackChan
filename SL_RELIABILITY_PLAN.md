# SL.BUS reliability improvements — implementation handoff for Luna

Status: **plan only**. Prepared 2026-09-20 against `b59542a`
(`Fix SL departure matching and record hardware validation`). This document
does not claim that the improvements below have been implemented or tested.

## 1. Implementation contract

Implement the four changes in sections 3–6 in numbered order, adding the
specified regression coverage at each step, then complete section 7. Work in
`/home/qyuan/workspace/StackChan`; inspect the worktree before editing and
preserve unrelated changes. Prefix every shell invocation with `rtk`; use
`rtk proxy` when a specialized wrapper is unavailable.

The intended behavior is:

- Transient transport failures receive at most one quick retry. Server
  cooldowns and persistent response errors receive normal backoff.
- A stop-specific failure does not put the other stop on a multi-minute
  refresh interval. Requests remain sequential in one worker.
- Home acknowledges closing while the worker drains; the Mooncake update
  loop continues to service the UI. Cleanup precedes returning to Launcher.
- Unchanged departure data is not copied on every UI tick. Expiry still
  takes effect at 90 seconds even without a new network result.

Keep the existing two routes, exact destination matching, immediate initial
fetch when no server cooldown applies, 30-second successful polling,
90-second data expiry, 32 KiB response limit, and compact two-panel layout.
Keep manual Gesture launch, its recognition/transition behavior, factory apps,
camera ownership, models, dependency pins, selective compiler optimization,
and the custom-firmware OTA safeguard.

Do not change `firmware/partitions.csv`, `firmware/dependencies.lock`,
`firmware/repos.json`, or the dependency/SDK configuration for this work.
Do not patch fetched dependencies. The existing `Http` transport stays in
use; replacing it is a separate task. No new model, library, server, mobile-app
change, settings screen, or transit feature is needed. Appendix A contains
unselected product ideas, not implementation instructions.

This handoff authorizes source changes and host/firmware validation when an
implementer is asked to execute it. It does not by itself request flashing,
committing, pushing, or publishing; follow the user's active instructions for
those operations. Missing hardware must not prevent source/build completion.

## 2. Baseline and source map

At review time, local `main` was six commits ahead of upstream
`1b5765599fba8aaad1811d9a79358ccc7051f5f3`; `b59542a` was one commit ahead
of the published fork. Recheck these facts before implementation.

The reviewed binary was 5,099,120 bytes in a 5,177,344-byte (`0x4f0000`)
application slot: **78,224 bytes / 76.4 KiB free**. Its SHA-256 was
`ca5b00269b7a5c26df21a99970177371ab92309c6b0ae36296b6354a9a872cda`.
The existing five host tests passed with warnings-as-errors and UBSan during
the review. The recorded 24-hour soak passed for the old behavior, including
5,532 successful fetches. These results do not validate future edits.

Read these production sources before changing them:

| Area | Sources and purpose |
| --- | --- |
| Fetching | `firmware/main/apps/app_sl_bus/sl_bus_departure_client.{h,cpp}`: currently flattens failures into `Failed`; reads HTTP status and delta-seconds `Retry-After`. |
| Scheduling and shutdown | `firmware/main/apps/app_sl_bus/app_sl_bus.{h,cpp}`: currently retries every failure, shares one failure counter, and joins synchronously in `onClose()`. |
| Data | `firmware/main/apps/app_sl_bus/sl_bus_departures.{h,cpp}`: route configuration, parsing, store, expiry and existing 60/120/300-second backoff helper. |
| View | `firmware/main/apps/app_sl_bus/view/view.{h,cpp}`: already avoids rendering unchanged generation/expiry/network state. Preserve this guard. |
| Lifecycle reference | `firmware/main/apps/app_gesture/app_gesture.cpp`, `gesture_transition.hpp`, and `firmware/main/apps/app_launcher/deferred_app_launcher.hpp`: completion semaphore, asynchronous drain, and close-before-launch. |
| Network reference | `firmware/main/hal/hal_network.cpp` and `firmware/managed_components/78__esp-ml307/src/{http_client.cc,esp/esp_ssl.cc}`: read to understand cancellation and timeout limits; leave their implementations unchanged. |
| Tests/build | `firmware/tests/CMakeLists.txt`, existing SL/Gesture/launcher tests, and `firmware/build_gesture.sh`. |

Important limitation: `Http::SetTimeout(5000)` controls individual HTTP waits.
It does not establish a five-second total transaction or TLS-connect deadline.
This plan fixes app-loop responsiveness during shutdown, not the underlying
transport's worst-case completion time. Never force-delete its worker or
call `Http::Close()` concurrently from the UI thread.

## 3. Classify failures and make quick retry precise

### Interfaces and test seam

Extend the existing internal `FetchResult` with:

```cpp
enum class FetchFailureKind { None, Transport, HttpStatus, InvalidResponse, Resource };
// Additional FetchResult fields:
FetchFailureKind failure_kind = FetchFailureKind::None;
int http_status = 0;  // 0 when no valid HTTP status was received
```

Keep `FetchResultStatus::{Success,Failed,Cancelled}`, `data`, `error`, and
`retry_after_seconds`. `None` applies to successful/cancelled results.
Error text remains a short user-facing message; status codes and failure
classes belong in serial diagnostics.

Make the actual client testable with a scripted implementation of the existing
`Http` interface. Add constructor injection:

```cpp
using HttpFactory = std::function<std::unique_ptr<Http>()>;
DepartureClient(CancelPredicate cancelled, HttpFactory make_http);
using RetryWait = std::function<bool(uint32_t delay_ms)>; // true means cancelled
FetchResult fetch_with_retry(const StopConfig&, const RetryWait&) const;
```

Retain `fetch()` as one attempt. Move the board-dependent HTTP factory into
`sl_bus_http_factory.cpp`; production uses `Board::GetInstance().GetNetwork()`
and `CreateHttp(0)`, with null checks. Declare that factory in the client
header and pass it explicitly from `AppSlBus`. Keep ESP/board includes out of
the client's core implementation. Host tests link that implementation and
the pinned component's lightweight `include/http.h`, excluding the board
factory. Reuse the existing `Http` interface; do not create a transport stack.

### Failure and retry policy

| Result | Classification | Quick retry |
| --- | --- | --- |
| Failed open, invalid/nonpositive status, or negative body read | `Transport` | Once, after an interruptible 250 ms wait, only with no positive `Retry-After` |
| Positive HTTP status other than 200 | `HttpStatus`, preserve the code | Never |
| Oversized response or parser rejection | `InvalidResponse` | Never |
| HTTP factory returns null | `Resource` | Never |
| Cancellation | `Cancelled` / `None` | Never; no failure publication |
| Valid 200, including empty departures | `Success` / `None` | Not needed |

The first result must be inspected before starting a second attempt. A
positive parsed `Retry-After` prevents quick retry; do not overwrite it with
a later result. Keep the existing delta-seconds parser, including saturation
at `UINT32_MAX`; HTTP-date parsing is outside this change. Convert seconds
to milliseconds only after widening to `uint64_t`.

`fetch_with_retry()` must call the injected wait and recheck cancellation
before its sole retry. `AppSlBus` supplies the semaphore-based interruptible
wait described below. Remove the unconditional retry and `delay(250)` from
the worker. Publish only the final logical result. Count a failed logical
fetch once in the applicable backoff counter from section 4, even when two
attempts failed. Log quick retries and final failures distinctly, including
stop and HTTP status where available.

## 4. Schedule each stop independently

Add `sl_bus_poll_schedule.{h,cpp}` with ordinary C++17 code and no RTOS/UI
dependencies. Production and host tests must use this same implementation.
Use the following contract:

```cpp
class PollSchedule {
public:
    void begin_session(uint64_t now_ms);
    std::optional<std::size_t> next_due(uint64_t now_ms) const;
    uint64_t next_wakeup_ms() const;
    void record_success(std::size_t stop, uint64_t completed_ms);
    void record_failure(std::size_t stop, const FetchResult&, uint64_t completed_ms);
};
```

Maintain a failure count and next-due monotonic timestamp per stop, plus one
shared server-cooldown deadline and throttle-failure count. Store this object
on `AppSlBus`, not as a new local on each worker start. The worker owns its
updates; session reset happens only after the previous worker has joined.

- `begin_session()` makes both stops immediately due and clears their local
  failure counts. Preserve an unexpired server cooldown and its throttle
  count across app close/reopen. Do not persist cooldowns to flash.
- Select the earliest due stop, breaking ties by stop index. `next_due()`
  returns empty during a shared cooldown. `next_wakeup_ms()` returns the
  maximum of the earliest per-stop due time and the shared cooldown deadline.
- Success resets that stop's failure count and schedules it for completion
  time + 30,000 ms. It also resets the shared throttle-failure count; it must
  not clear a cooldown deadline already established for the future.
- An ordinary final failure schedules only that stop for completion time +
  60, 120, then 300 seconds, using the existing helper. Saturate counters at
  the highest meaningful backoff step. The other stop keeps its own schedule.
- Treat HTTP 429, and any failed response carrying positive parsed
  `Retry-After`, as a shared cooldown for these two URLs on the same origin.
  Increment the separate throttle count and use the maximum of its
  60/120/300-second backoff and the server delay. Never shorten an existing
  cooldown. For this case leave per-stop due timestamps and failure counts
  unchanged; the shared deadline gates both stops. HTTP 503 without
  `Retry-After` uses ordinary per-stop backoff.
- Cancelled results do not update either schedule or any failure counter.
- A cooldown found while fetching the first stop must prevent the second
  request in that worker iteration. Publish `SL busy; retry later` for stops
  postponed by a shared cooldown, retaining cached data. Do not increment
  the unrequested stop's failure counter. Publish this state only on entry
  to a cooldown/session, not on every waiting tick.

The worker repeatedly selects one due stop, marks that stop fetching, ensures
network availability, performs `fetch_with_retry()`, publishes the result,
and records the next deadline. Set `_network_waiting` only during the network
availability check, so the selected stop can show `Connecting`. Reevaluate
due stops after each result.
Do not mark all stops `Fetching` at the top of every loop. After a transport
failure, another due stop may run; after a shared cooldown, none may run.
Check `_stop` after every blocking operation and immediately before publishing
a result or starting another request. Cancellation leaves the schedule alone.

Retain sequential I/O to protect internal heap. The healthy stop's *scheduled*
interval is 30 seconds after success; actual execution can be delayed by a
currently running request. Do not promise exact wall-clock intervals while
using a synchronous, shared transport.

Bound SL's Wi-Fi startup wait by passing a cancellation predicate combining
`_stop` with a 15-second monotonic deadline to `startNetworkCancellable()`.
Check `_stop` separately on return: Home exits without publishing a failure;
deadline expiry publishes `Wi-Fi unavailable` and records one ordinary
failure for each currently due stop. A startup timeout is not a quick HTTP
retry. Keep the board-owned connection/provisioning machinery intact. This
predicate bounds the helper's polling wait, not blocking calls inside the
board implementation or a later TLS handshake.

Replace the seconds-only worker wait with `wait_interruptibly_ms(uint64_t)`.
Use `_wake`, slices of at most 1,000 ms, and elapsed monotonic time to compute
remaining duration. Home wakes both the 250 ms retry wait and long backoff
immediately. Waking without `_stop` set must not invent a cancellation.

## 5. Drain SL.BUS asynchronously on Home

Add a small UI-owned `SlBusCloseState` helper in `sl_bus_close_state.hpp`:
states `Running`, `Draining`, `CloseRequested`; `request_home()` emits a stop
action once; `worker_completed()` emits a close action once, only while
draining; `reset()` returns to `Running` on app open after the prior join.
Keep it independent of ESP-IDF/LVGL so tests can drive event order.

- Change the Home callback to a stop-intent method. On its first call set
  `_stop`, give `_wake`, record the drain start, and enter `Draining`.
  Repeated Home events have no further effect.
- Process Home before updating departure content. While draining, keep
  servicing Home/status UI, and poll `_done` with **zero timeout** outside
  `LvglLockGuard`. No worker started counts as already completed.
- Add `SlBusView::show_closing()` to set `Closing...` once in the first
  panel's existing message label. Keep it visible by skipping departure
  rendering while draining; no extra page or repeated status labels.
- A successful semaphore take sets `_worker_started=false` immediately.
  Once completion is observed, call `close()` exactly once. `onClose()` is
  still the single owner of semaphore/view/Home/status cleanup.
- Retain a blocking join outside the LVGL lock as the destructor/externally
  forced-close fallback. Normal Home uses the asynchronous path and must
  not take the consumed completion semaphore a second time.
- Keep the existing worker rule: release local HTTP/parser resources before
  signaling `_done`, and never access app members after signaling it.
- If draining exceeds five seconds, emit one diagnostic warning and keep
  servicing the UI until completion. Do not signal fake completion, delete
  the task, free its resources, or open another app on a timer.
- Reopening clears stop/session/UI cache state only after the previous
  worker is fully joined. Launcher bookkeeping and Gesture's deferred
  launch implementation remain unchanged.

Acceptance distinguishes acknowledgment from completion: show closing within
100 ms under normal UI load; verify that UI updates continue during an
artificially delayed request. Record actual Home-to-Launcher latency on
hardware. This plan does not claim that latency is always below five seconds.

## 6. Copy departure data only when it changes

Add these data-layer interfaces:

```cpp
bool DepartureStore::copy_if_changed(DepartureSnapshot& output, bool force = false) const;
void update_expiry(DepartureSnapshot& snapshot, uint64_t now_ms);
```

Under the store mutex, compare the output's generation with the stored
generation. Return false without assigning strings when unchanged; otherwise
copy and return true. `force` always copies. Preserve the existing generation
increment on every data/fetch-status/error publication.

`AppSlBus` owns one UI-thread cached snapshot. After `_store.reset()` on open,
force a first copy. On normal running ticks call `copy_if_changed()`, then
`update_expiry()` on that cache outside the mutex. Pass it to the existing
view guard with `_network_waiting`. The expiry helper must recompute both
true and false states from `last_success_ms` and the existing 90-second rule.
It changes neither generation nor the stored source data, and allocates no
memory. Preserve the current zero-timestamp and backward-time behavior.

Keep `snapshot(now_ms)` as a compatible convenience wrapper for existing
tests/callers; implement it using a forced copy plus the shared expiry helper.
The SL app hot path must use `copy_if_changed()`. Force the copy on every new
session so generation reset cannot leave old-session data visible.

Do not remove `SlBusView`'s generation/expiry/network-state render guard.
An expiry change alone must redraw even when no network result arrives.
This is an allocation/copy optimization, not a promised battery-life gain.

## 7. Regression tests, validation, and delivery

### Required host coverage

Register new tests in the existing `firmware/tests/CMakeLists.txt`. Use fake
time and a scripted `Http` implementation; never sleep or contact SL in host
tests. Exercise the real client/retry/scheduler/close helper implementations,
not a duplicate of their intended logic.

| Area | Required cases |
| --- | --- |
| Client/retry | Transport failure then success: exactly two attempts and one 250 ms wait. Two failures: no third attempt. 404/429/503, malformed JSON, oversized body and null factory: no quick retry. Positive `Retry-After`: retained and no quick retry. Cancellation before request, during retry wait and between body reads: no later request or success publication. Close HTTP resources on all applicable return paths. |
| Scheduling | Both stops initially due. Independent 30-second success deadlines. At least ten simulated minutes with one stop persistently failing: its intervals become 60/120/300 seconds while the healthy stop keeps 30-second deadlines. One stop's success cannot reset the other's failure count. |
| Cooldown | 429 without header: shared backoff. 503 or another failure with `Retry-After: 600`: neither stop starts before the shared deadline. First-stop cooldown prevents a second-stop request. Reopen preserves remaining cooldown. Repeated cooldowns never shorten it. Success with an empty list is a success. Large/saturated retry seconds do not overflow milliseconds. |
| Closing | Home during normal running, a retry wait, and an unfinished request; duplicate Home; completion before/after Home; no worker; ten reset/reopen cycles. No close action before completion, and only one close afterward. Document separately that host helper tests do not exercise real RTOS semaphores or TLS. |
| Snapshot | First/forced copy; unchanged call returns false without replacing output; success/failure/fetching updates propagate; 89,999 ms fresh versus 90,000 ms expired; expiry without generation change; recovery to fresh; stop independence; reopen after generation reset. |

Keep all five existing tests. Build with assertions enabled, warnings as errors
and UBSan. Do not use a Release build that compiles out the `assert` checks.
For the copy optimization, verify the unchanged path with long notice strings
and allocation counting around that path only, so small-string optimization
does not hide a regression. Require zero allocations there; no speedup claim
is needed.

Run from the repository root, with CMake/Ninja on PATH:

```bash
rtk proxy cmake -S firmware/tests -B /tmp/stackchan-sl-reliability-tests -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=all" -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=undefined
rtk proxy cmake --build /tmp/stackchan-sl-reliability-tests -j2
rtk proxy ctest --test-dir /tmp/stackchan-sl-reliability-tests --output-on-failure
rtk git diff --check
```

Complete the build before running CTest. If tools are absent from PATH, check
`/home/qyuan/miniconda3/envs/pipeline-study/bin` for CMake, CTest and Ninja.
For Ninja configuration, `-DCMAKE_MAKE_PROGRAM=<absolute ninja path>` avoids
changing the environment. Treat these locations as hints to verify, not
portable requirements.

### Firmware and hardware acceptance

Use ESP-IDF **5.5.4** and the existing build helper. The current local toolchain
hints are `/home/qyuan/Documents/Codex/2026-09-11/i-x20/work/esp-idf` and its
sibling `idf-tools` directory; inspect `firmware/build/CMakeCache.txt` and
activate that environment with the appropriate `IDF_TOOLS_PATH`. Do not
declare a missing toolchain based only on PATH. Keep full logs in `/tmp` and
show concise errors/results.

```bash
rtk proxy bash -c 'cd firmware && ./build_gesture.sh > /tmp/stackchan-sl-reliability-build.log 2>&1'
rtk proxy tail -n 20 /tmp/stackchan-sl-reliability-build.log
rtk proxy python3 firmware/tests/check_gesture_link_map.py firmware/build/stack-chan.map
rtk proxy stat -c '%s' firmware/build/stack-chan.bin
rtk proxy sha256sum firmware/build/stack-chan.bin
rtk git diff -- firmware/partitions.csv firmware/dependencies.lock firmware/repos.json firmware/main/idf_component.yml firmware/sdkconfig.defaults
```

The helper must exit successfully; an old binary or successful `tail` command
is not build validation. Record fresh image size, free bytes, SHA-256, link-map
result and `esptool image_info` checksum/hash result. Require application size
at or below `0x4f0000` and all 12 hand convolution symbols supplied by ESP-DL.
Report the size delta against the verified 5,099,120-byte baseline. If the
image does not fit, reduce only this change; do not move partitions, remove
factory features or change models/pins to accommodate it.

When device testing is authorized and hardware is present, run:

1. Both routes online; one-stop failure; offline startup, loss and recovery;
   expiry and recovery; Home during each relevant worker phase. Use a
   controlled fault source/test transport for 429/503/delayed responses;
   never try to trigger throttling by hammering the live SL API.
2. Ten `Launcher -> Gesture -> SL.BUS -> Home -> Launcher` cycles, including
   Home cancellation during Gesture draining. Confirm camera reuse and
   close-before-open ordering. Sample free internal heap and largest free
   block at matching lifecycle points after a short idle settling interval;
   investigate any sustained downward trend beyond baseline noise.
3. A fresh 24-hour powered SL.BUS soak before claiming long-run reliability
   for the changed code. Review final fetch failures, retry/cooldown behavior,
   heap trend, panic/assert/watchdog/brownout/reset markers.

Record unperformed hardware checks as **PENDING**. If flashing is requested,
first verify device/partition/OTA selection; use the existing application-only
workflow at `ota_0` / `0x20000`, preserving factory `ota_1` and all other
regions. This plan is not a full-flash instruction.

### Documentation and completion report

After implementation, add a dated status/result section here. Update the
opening status of `GESTURE_PROTOTYPE.md` and `SL_INTERACTION_PLAN.md` so readers
immediately see that Gesture-to-SL integration exists. Keep older size/timing
and soak measurements clearly labeled as historical; retain their evidence.
Do not present a reused firmware version string as proof of installed code;
identify validation by commit and image hash.

The implementation handoff must list changed behavior, tests actually run,
fresh firmware size/hash, protected-file check, and remaining hardware checks.
Keep the diff focused on this work. Do not report complete hardware acceptance
from host tests or the earlier soak. Estimated effort: 2–4 engineering days
for source/build work, plus device testing and the elapsed soak interval.

## Appendix A. Product directions to discuss separately

The user prefers their phone for detailed information and transit-management
features. Future StackChan work should make its physical response worthwhile:
expressive eyes, deliberate motion, tactile interaction and brief social play.
These ideas are **not selected for implementation** by this handoff.

The baseline already has blinking, breathing, random idle motion/expression,
and happy head-pat responses with hearts and movement. Inspect
`firmware/main/stackchan/modifiers/{blink,breath,idle_motion,idle_expression,head_pet}.h`
before proposing these as new capabilities. Hardware capabilities are also
described in the [upstream overview](https://github.com/m5stack/StackChan).

| Direction | Concrete experience | Why it fits / feasibility |
| --- | --- | --- |
| Coordinated personality | Eyes glance first, the head follows, an expression resolves, then the robot settles. Calm/curious/playful temperaments change the timing and frequency. | Builds on existing modifiers with coherent choreography and interaction priority. No camera model or cloud latency needed. |
| Turn-taking rhythm game | StackChan nods once or twice; the user answers with the same number of head taps. It adds a beat, celebrates a match, and gives a playful retry reaction. | Touch timing and physical turn-taking are the experience. Start with taps/nods; keep sequences short and allow immediate exit. |
| Gesture as a social exchange | A deliberate thumbs-up gets a grin and one pleased nod; the robot settles after the exchange. | Reuses the working classifier but needs a separate product decision about navigation. Recognition remains roughly one-second-scale; do not promise instant gestures. |
| Learn a little performance | In a local rehearsal mode, the user selects and approves short eye/head/LED sequences, then recalls a favorite with a touch gesture. | Small reusable motion scripts could give the robot a distinctive character. Reuse bounded servo APIs; never record poses by forcing powered servos by hand. |
| Peek-a-boo | The robot responds to deliberate camera covering and uncovering with anticipation and surprise. | A separate feasibility spike using simple image statistics may avoid a new model. Must distinguish occlusion from ordinary darkness before any product claim. |

The first two are the strongest candidates for an initial experience study.
Prototype one short interaction before adding menus or broad autonomy. For
touch, target a visible acknowledgment within 100 ms, then let the servo motion
unfold naturally. Repeated inputs should not queue a long sequence; give the
user a way to interrupt, settle and leave. Camera-based eye contact, room-scale
presence, continuous mirroring and new models require separate latency,
false-trigger, camera-ownership and memory measurements.
