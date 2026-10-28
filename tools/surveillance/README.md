# StackChan Surveillance receiver

The receiver stores images under `~/.local/share/stackchan-surveillance/`, indexes them in SQLite, and serves the gallery only on `127.0.0.1:8765`. Its HTTPS API listens on the laptop LAN at port `8443` and announces `stackchan-surveillance.local` over mDNS. It has no cloud endpoint or redirect handling.

## Provision this laptop

Run from the repository root. This creates a private local CA, HTTPS certificate, random pairing token, and an ignored firmware profile at `firmware/main/apps/app_surveillance/connection_profile.h`:

```bash
rtk proxy python3 tools/surveillance/provision.py
```

Install the per-user service and its Python dependencies when you are ready to run the receiver:

```bash
rtk proxy python3 tools/surveillance/provision.py --install-service
```

The service uses `Restart=always`; its API binds to the LAN for device uploads and its gallery binds only to laptop localhost. Open `http://127.0.0.1:8765`. For a different checkout, pass `--profile /path/to/firmware/main/apps/app_surveillance/connection_profile.h`. Keep the generated config, token, private key, CA key, and profile private. Build the firmware after provisioning so the token and CA are included in that device image.

If the laptop's network blocks multicast DNS, configure `stackchan-surveillance.local` in the router's local DNS or hosts service to point to the laptop's current LAN address. TLS still verifies the `.local` name and the provisioned private CA.

## Build and install on StackChan

Provision first so the ignored `connection_profile.h` is present for the firmware build. Then run a fresh build with ESP-IDF 5.5.4:

```bash
rtk proxy bash -lc '
set -e
export IDF_TOOLS_PATH=/home/qyuan/Documents/Codex/2026-09-11/i-x20/work/idf-tools
source /home/qyuan/Documents/Codex/2026-09-11/i-x20/work/esp-idf/export.sh
rtk proxy idf.py -C firmware reconfigure
rtk proxy idf.py -C firmware build
'
```

Before flashing, verify the connected board and its partition table, confirm it boots from `ota_0`, and check that `firmware/build/stack-chan.bin` fits the app partition. Replace `PORT` with the device's stable USB serial path. This writes only the app image at `0x20000`:

```bash
rtk proxy bash -lc '
set -e
export IDF_TOOLS_PATH=/home/qyuan/Documents/Codex/2026-09-11/i-x20/work/idf-tools
source /home/qyuan/Documents/Codex/2026-09-11/i-x20/work/esp-idf/export.sh
rtk proxy python -m esptool --chip esp32s3 -p PORT -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x20000 firmware/build/stack-chan.bin
'
```

The app opens disarmed. Supervise the setup sweep with the cable clear and the base stable, inspect each view and the released-motor position, then confirm setup before arming. Complete the 24-hour powered soak and offline recording checks before unattended use.

## API contract

- `PUT /v1/frames/{capture_id}` accepts a raw JPEG and capture metadata in `X-Capture-*` headers. The pairing token is `X-StackChan-Token`. Acknowledgement follows file fsync, atomic rename, and SQLite commit. Repeating the same id and bytes is safe; reusing an id for different bytes returns conflict.
- `POST /v1/heartbeat` reports device state and receives receiver UTC time plus storage status. Scan group transitions (`scanning`, `complete`, `failed`) are persisted in SQLite and shown in the gallery, including Home cancellations.
- The device and gallery distinguish a captured or queued image from a disk-confirmed image and display current device status separately from receiver storage errors.
- The gallery marks the device disconnected after 15 seconds without a heartbeat. Times are displayed in `Europe/Stockholm`.
- Retention removes complete groups oldest-first when they exceed 30 days or 5 GB (5,000,000,000 bytes). A storage error stops image acknowledgements and remains visible in the gallery and subsequent heartbeats; after fixing the underlying disk issue, restart the service to clear its failed-storage state.

## Local validation

```bash
rtk proxy python3 -m unittest discover -s tools/surveillance -p 'test_*.py' -v
rtk proxy systemctl --user status stackchan-surveillance.service
```

These tests cover metadata/path validation, duplicate uploads, partial file/index writes, retention, heartbeat time anchoring, disconnect state, visible storage errors, persisted incomplete scan records, queue limits and Home cancellation, and generated C++ profile escaping. `aiohttp` and `zeroconf` are runtime dependencies listed in `requirements.txt`.
