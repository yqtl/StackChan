# Local gesture prototype

This branch extends StackChan factory firmware 1.5.1 with a `GESTURE` app. The
app displays the live 320 x 240 camera feed and runs Espressif's hand detector
and gesture classifier locally on the ESP32-S3. It does not send camera images
to an API or cloud service.

The current milestone recognizes a single thumbs-up held for one second and
rearms after the hand leaves the frame for one second. The screen reports hand
count, the best detector score when multiple candidates exist, gesture label,
classifier confidence, and end-to-end frame time. It also draws the strongest
hand box in green. The future SL line 57 lookup is intentionally left as a
commented integration point after gesture confirmation.

## Flash layout

The factory application partitions remain unchanged:

- `ota_0`: custom `1.5.1-gesture.4` application at `0x20000`
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
`-O3`; surrounding code uses the project's `-Os`. This build tests whether the
all-`-Os` configuration caused the saturated false detections, while still
fitting the original OTA slot.

The application image is 4,918,720 bytes. It leaves 258,624 bytes free in the
5,177,344-byte application slot.

## Validation status

- Full ESP-IDF 5.5.4 build passes.
- Application and model images fit their partitions.
- The application image checksum and validation hash pass.
- Host gesture-gate tests pass with warnings as errors and undefined-behavior
  checks enabled.
- `1.5.1-gesture.4` was written to `ota_0` and esptool verified the flash hash.
- Empty-scene and thumbs-up behavior on `.4` still require hardware testing.

## Continue development

Start by opening `GESTURE` on the flashed StackChan and observing an empty
scene. Record the hand count, best confidence, inference time, and whether a
green box appears. Then show one thumbs-up and record its label and confidence.

Versions `.2` and `.3` consistently returned ten hands at 100% confidence on
empty scenes, with about 425 ms per frame. Raising the detector threshold to
60% and passing the camera's native YUYV data directly did not change that
result, so further threshold tuning is unlikely to help. Version `.4` restores
upstream optimization for the quantized core while retaining a size-optimized
build elsewhere. If `.4` still produces ten saturated detections, the next
diagnostic should run the detector on a synthetic uniform image and report raw
pre-postprocessor tensor ranges. That will separate camera input problems from
model execution or postprocessing problems.

After local thumbs-up recognition is reliable, enqueue the network action at
the `THUMBS_UP_CONFIRMED` point in `app_gesture.cpp`. The intended final action
is an SL API lookup for approaching line 57 buses toward Hjorthagen. Keep that
request off the camera worker so network latency cannot freeze the preview.
