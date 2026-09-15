<proposed_plan>
# Tullgårdsparken and Bohusgatan bus-time app

## Summary

Add an **SL BUS** launcher app displaying **57 toward Hjorthagen from Tullgårdsparken** and **74 toward Hornsberg from Bohusgatan**. It stays visible and refreshes while open; normal startup remains unchanged.

Deliver this independently of gesture recognition. Keep person-presence activation as a later experiment targeting 1–2 metres.

## Display and SL data

- Use verified **site 1314**, bus line **57**, **direction code 2**, and **site 1318**, bus line **74**, **direction code 2**. The live response’s `direction` text can be misleading, so filter using the code and display each departure’s actual `destination`. [Verified Tullgårdsparken departures](https://transport.integration.sl.se/v1/sites/1314/departures).
- Fetch both `/v1/sites/{siteId}/departures?transport=BUS&forecast=60` endpoints immediately on opening, then every **30 seconds**. SL Transport requires no API key. [Official documentation](https://www.trafiklab.se/api/our-apis/sl/transport/).
- Show the two stops in fixed side-by-side panels. Use a larger primary time, up to two fixed subsequent rows, and SL’s `display` text directly, preserving its minutes/clock-time format.
- Do not animate or scroll labels. Clip text within its panel to keep the display calm and information-dense. Show the route destination once when all departures share it; show per-departure destinations only when they differ. Keep the successful state quiet and reserve the compact bottom message for connection, freshness, cancellation and disruption information.
- After a failed refresh, mark retained information as outdated. After **90 seconds without success**, replace departure values with a compact unavailable state until recovery.

## Implementation

- Add `AppSlBus` using the existing Mooncake lifecycle, LVGL locking, status bar and home control. Keep the display active while open; closing returns to the launcher and stops polling.
- Separate the departure client/parser from presentation. Publish bounded snapshots containing departures, notices, fetch status and a monotonic last-success timestamp. This allows future presence activation without rewriting data retrieval.
- Reuse existing Wi-Fi credentials and HTTP infrastructure with certificate validation. Make network startup cancellable for this app; the current indefinitely waiting `startNetwork()` must not prevent closing while offline. Preserve existing callers’ behavior.
- Run requests in a worker, with a five-second network timeout, bounded response storage and interruptible retry waits. Back off failures to 60, 120, then 300 seconds; respect longer server retry instructions. Resume normal polling after success.
- Keep camera inference inactive in the bus app. Preserve the Gesture app, factory apps, existing model partitions and factory recovery image.

## Validation and completion criteria

- Parser tests cover both configured lines and directions—including misleading direction text—other lines, missing fields, cancellations, notices, empty results and malformed responses.
- Lifecycle tests cover offline opening, failed/slow requests, closing during connection, repeated reopening, freshness expiry and recovery.
- Hardware checks verify Swedish characters, layout and readability at 1–2 metres, correct departures, responsive home navigation and Wi-Fi reconnection.
- Run a **24-hour powered soak test** for crashes, watchdog resets, resource leaks and stale-data handling.
- Build with existing regression checks and verify application size against the unchanged OTA slot. Before any hardware update, verify the device’s partition/boot state and use only the intended application partition.

Presence detection and further gesture tuning are deferred from this implementation. The first milestone is a dependable bus display that works without either.
</proposed_plan>
