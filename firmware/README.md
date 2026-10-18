
## Build

### Fetch Dependencies

```bash
python3 ./fetch_repos.py
```

### Tool Chains

[ESP-IDF v5.5.4](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/index.html)

### Build

```bash
idf.py build
```

### Leaving out factory apps

AVATAR, ESPNOW.REMOTE, APP.CENTER, EZDATA and DANCE can each be left out of
the build, together with the HAL service that only that app uses. All five are
on by default. Switch them off under `StackChan factory apps` in
`idf.py menuconfig`, or add these lines to `sdkconfig.defaults.local` and
delete `sdkconfig` once so the defaults are read again:

```
CONFIG_STACKCHAN_APP_AVATAR=n
CONFIG_STACKCHAN_APP_ESPNOW_REMOTE=n
CONFIG_STACKCHAN_APP_APP_CENTER=n
CONFIG_STACKCHAN_APP_EZDATA=n
CONFIG_STACKCHAN_APP_DANCE=n
```

The build log's `binary size` line shows how much of the app partition is
left. `idf.py size-components` lists the libraries that take the most space.

### Host-side tests

The motion coordinate helpers can be tested without ESP-IDF hardware:

```bash
cmake -S tests -B build-host-tests
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure
```

### Flash

```bash
idf.py flash
```
