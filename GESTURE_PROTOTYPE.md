# Local gesture prototype

This branch extends StackChan factory firmware 1.5.1 with a `GESTURE` app. The
app displays the live 320 x 240 camera feed and runs Espressif's hand detector
and gesture classifier locally on the ESP32-S3. It does not send camera images
to an API or cloud service.

The current milestone confirms a single thumbs-up after at least two fresh
positive observations spanning 250 ms and rearms after the hand leaves the frame
for one second. The screen reports hand
count, the best detector score when multiple candidates exist, gesture label,
classifier confidence, and end-to-end frame time. It also draws the strongest
hand box in green. The future SL line 57 lookup is intentionally left as a
commented integration point after gesture confirmation.

## Bounding-box fix in .7

In `.6`, publishing a fresh camera image overwrote the green box drawn into
the pixel buffer. Detection redrew it about 365 ms later, causing a blink on
every recognition cycle, especially while classification was skipped.

`.7` renders the box as a transparent LVGL child of the preview instead. The
last detected box remains visible while the next image is captured and analyzed.
Its position and size change when a new detection arrives, and it hides when
the detector reports no valid hand box. Coordinates stay clipped to the preview.
The overlay ignores touch input and is deleted with the preview on app close.
Recognition thresholds, confirmation timing and model execution are unchanged.
The box can still lag hand motion by one detection cycle, and actual detection
dropouts still hide it; this fix removes the rendering-induced blink.

The `.7` ESP-IDF build passes. Its 5,034,576-byte application leaves 142,768
bytes free in the unchanged OTA slot. The image checksum and validation hash
pass, and all 12 hand convolution entry points still link to ESP-DL. On
2026-09-13, the device partition table and valid `ota_0` boot selection were
rechecked, and only `.7`'s application range was written at `0x20000`. Esptool
verified the written-image hash and reset the device. Boot logs confirm
`1.5.1-gesture.7`, and the runtime capture includes two successful thumbs-up
confirmations with no crash, assertion, stack-canary or watchdog markers.
The user confirmed that the box is steady during the hardware check. Factory
`ota_1` and all other partitions were left untouched.
Sixteen sampled classified frames had a median processing time of 891 ms;
eleven detector-only samples had a median of 491 ms. These are observed samples,
not a controlled comparison of identical scenes with `.6`.

## Flash layout

The factory application partitions remain unchanged:

- `ota_0`: custom `1.5.1-gesture.7` application at `0x20000`
- `ota_1`: original factory 1.5.1 application at `0x510000`
- `hand_det`: local hand detector model at `0xe10000`
- `hand_gesture_cls`: local gesture classifier model at `0xe90000`

Automatic stock firmware replacement is disabled in this custom build. A
manual factory restore through M5Burner remains available.

## Build

On a new machine, install and activate ESP-IDF 5.5.4, then run:

```bash
git clone https://github.com/yqtl/StackChan.git
cd StackChan
. /path/to/esp-idf/export.sh
cd firmware
./build_gesture.sh
```

The helper fetches the pinned nested repositories, resolves managed
dependencies, applies both checked-in patches idempotently, and builds the
firmware. ESP-DL's quantized kernels and module wrappers retain upstream
`-O3`; surrounding code uses the project's `-Os`. The helper also checks the
link map to ensure hand convolution routines come from ESP-DL.

Keep build output in a file when working through Codex:

```bash
rtk proxy bash -c './build_gesture.sh > /tmp/stackchan-gesture-build.log 2>&1'
rtk proxy tail -n 12 /tmp/stackchan-gesture-build.log
```

For host regression tests, use an activated ESP-IDF environment (which includes
CMake) and keep the output similarly bounded:

```bash
rtk proxy cmake -S tests -B /tmp/stackchan-gesture-tests -DCMAKE_CXX_FLAGS="-Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=undefined" > /tmp/stackchan-gesture-tests.log 2>&1
rtk proxy cmake --build /tmp/stackchan-gesture-tests >> /tmp/stackchan-gesture-tests.log 2>&1
rtk proxy ctest --test-dir /tmp/stackchan-gesture-tests --output-on-failure
```

ESP-SR is pinned to 2.4.5 to fix its ESP32-S3 kernel symbol collision with
ESP-DL. This requires ESP-DSP 1.8.0 and ESP-DL 3.3.2; other existing dependency
versions are retained in `dependencies.lock`.

The `.7` application image is 5,034,576 bytes. It leaves 142,768 bytes free in the
5,177,344-byte application slot.

## Performance changes in .6

- `gesture_models.hpp` retains the pinned models and their exact preprocessing,
  cropping and postprocessing. It explicitly requests `RUNTIME_MODE_AUTO` for
  both models, allowing ESP-DL to split large convolutions across both S3 cores.
  The upstream wrappers use the single-core default of `Model::run()`; see the
  [ESP-DL model API](https://docs.espressif.com/projects/esp-dl/en/latest/api_reference/model_api.html).
  Other factory applications keep their existing inference behavior.
- Both models load on app entry, so the first detected hand no longer triggers
  a classifier load in the middle of recognition.
- Gesture requests a fresh camera frame, discarding buffers queued during the
  previous inference without copying them. This removes an extra inference
  cycle of image age at the cost of waiting for a new sensor frame. Other camera
  callers keep the default capture behavior. Streaming reuses the PSRAM frame
  copy and no longer emits a warning for every successful capture.
- The preview is published before detection. Pixel writes and invalidation use
  the LVGL lock to prevent concurrent rendering of a partially updated buffer.
  The Gesture source uses `-O3` for its pixel conversion; the rest of the
  factory app retains size optimization. Preview refresh still follows the
  recognition loop; this is not a separate video stream.
- Confirmation uses a 250 ms debounce and at least two positive frames instead
  of a one-second hold. At the old 830 ms cadence this needs two frames instead
  of three: 830 ms from the first positive instead of 1660 ms. This is a gate
  calculation, not a measured .6 hardware speedup. The 85% classifier threshold,
  single-hand requirement, one-second removal interval, and 1200 ms stall
  cutoff remain. A shorter hold needs renewed testing against unintended poses.
- Once confirmed, classification is skipped until the hand has been removed;
  detection continues on every frame. The worker yields one RTOS tick instead
  of sleeping a fixed 30 ms.
- Once every five seconds, `Gesture` logs `Timing ms: capture=... preview=...
  detect=... classify=... total=... hands=... cls=... heap=...`. These are sampled
  frame timings, including capture and UI-lock waits; `cls=0` means classification
  was skipped. Compare `cls=1` single-hand samples with each other. Confirmation
  events remain immediate logs. Per-frame classifier logs have been removed.

## Validation status

### .6 build and hardware validation

- Full ESP-IDF 5.5.4 build passes. The application and both model images fit
  their unchanged partitions; the link-map guard passes all 12 hand convolution
  entry points, supplied by ESP-DL.
- Application checksum and validation hash pass (`esptool image_info`).
- Host CTest tests pass with warnings as errors and UBSan, covering the faster
  confirmation, repeated triggers, interruptions, stale frames, duplicate
  timestamps and clock reversal.
- Flashed `.6` to `ota_0` on 2026-09-13 after verifying that the device partition
  table exactly matched the build, `ota_0` was the selected valid boot partition,
  and `ota_1` held factory `1.5.1`. ESP32-S3 revision 0.2 and 16 MB flash were
  confirmed. Only the application range was erased/written; esptool verified
  its hash and reset the device. No bootloader, partition table, OTA selection,
  assets or models were written.
- Boot logs identify `1.5.1-gesture.6`. A three-minute serial capture contains
  two successful confirmations and no panic, assertion, stack-canary or watchdog
  markers. The user tested confirmation, hand removal and repetition, and
  reported that the response was noticeably faster.
- Eight sampled classified frames had a median processing time of 942.5 ms
  (941-958 ms). Ten detector-only samples had a median of 495 ms (493-500 ms).
  Classified-stage medians were capture 87 ms, conversion/preview 41 ms,
  detection 365 ms and classification 446 ms. Minimum sampled free internal
  heap was 112,563 bytes. These are five-second samples, not every frame.
- Raw classified-frame throughput did not improve against the earlier ~830 ms
  baseline. Fresh capture adds a sensor wait, and inference remains the main
  cost. The user-observed response improvement accompanies fresher images and
  the shorter confirmation requirement; motion-to-response latency was not
  instrumented. Do not interpret the 250 ms debounce as end-to-end latency.
  Further compute optimization should measure detector/classifier time
  separately and compare the same scene. Other-pose rejection and app
  close/reopen behavior were not revalidated in this .6 capture.

### .5 hardware baseline

- Full ESP-IDF 5.5.4 build passes.
- Application and model images fit their partitions.
- The application image checksum and validation hash pass.
- Host gesture-gate tests pass with warnings as errors and undefined-behavior
  checks enabled.
- The link-map guard rejects `.4` and passes `.5`: all 12 hand convolution
  entry points resolve to ESP-DL.
- The first `.5` hardware test reports no hand in an empty scene at about
  405 ms per frame. The earlier saturated detections are resolved.
- Hardware logs show sustained thumbs-up (`like`) at 100% confidence, with
  classified frames 830 ms apart. RGB and native YUYV gave the same 73.1%
  detector confidence on a paired frame. Recognition uses preview RGB.
- The old 750 ms stale-observation timeout reset the hold on every classified
  frame. It is now 1200 ms; regression tests reproduce the original failure
  and verify confirmation, rearming, and stall rejection at hardware cadence.
- The detector uses Espressif's 25% default. Confirmation still requires 85%
  classifier confidence and at least a one-second hold (about 1.7 seconds
  from the first positive frame at the measured cadence).
- The timing fix was flashed to `ota_0` and its hash verified. Both host CTest
  tests pass, and the gesture gate also passes with UBSan and warnings as errors.
  Hardware testing passed: the user confirmed thumbs-up after a three-second
  hold, clearing after lowering the hand, successful repeated confirmations,
  and no thumbs-up confirmation for other tested gestures. Earlier partial-box
  and second-candidate reports were not addressed by crop changes.

## Continue development

Validate `.7` on StackChan after checking the application size and flash plan.
Preserve `ota_1` and the factory partition table; an app-only update belongs at
`0x20000`. Open `GESTURE`, observe an empty scene, then show one thumbs-up, keep
it raised, lower it for one second, and repeat. Check other poses and two hands
do not confirm, and close/reopen the app to check camera ownership and UI
responsiveness. Capture several five-second timing samples in each state and
compare classified frames with the `.5` baseline (~830 ms). Monitor internal
heap and confirm that `cls=0` while latched. Measure actual motion-to-response
on the device as well as the processing time displayed on screen.

Versions `.2` through `.4` returned ten hands at 100% confidence on empty
scenes, with about 429 ms per frame. Threshold, input-format, and compiler
optimization changes did not fix it. The `.4` link map shows ESP-DL calling
`dl_tie728_s8_conv2d_*` routines from ESP-SR's legacy `libdl_lib.a`.
ESP-SR 2.4.5 gives its routines distinct `dl_tie728_sr_*` names, resolving
the collision. See the [upstream fix](https://github.com/espressif/esp-sr/commit/5fadfbd5bd9df756f543973bd43520d9270ffb1b).

The temporary raw tensor dumps and double-inference comparison pass have
been removed. The build guard can also be run directly:

```bash
rtk proxy python3 firmware/tests/check_gesture_link_map.py firmware/build/stack-chan.map
```

After local thumbs-up recognition is reliable, enqueue the network action at
the `THUMBS_UP_CONFIRMED` point in `app_gesture.cpp`. The intended final action
is an SL API lookup for approaching line 57 buses toward Hjorthagen. Keep that
request off the camera worker so network latency cannot freeze the preview.
