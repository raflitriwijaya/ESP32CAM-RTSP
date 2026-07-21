# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.0.0] - 2026-07-08 — Production Hardening Release

This release represents a systematic audit and hardening of the original
[rzeldent/esp32cam-rtsp](https://github.com/rzeldent/esp32cam-rtsp) codebase.
All changes are traceable to specific audit findings in AUDIT_REPORT_V1.md.

### Added
- 5-task FreeRTOS architecture (task_network, task_rtsp, task_camera, task_web_ui, task_health) per CLAUDE.md §5
- Camera quality mapping layer (`camera_map_quality`) — UI 1-100 to OV2640 driver 0-63
- WiFi state machine with exponential backoff (1s/2s/4s) and 5-second disconnect debounce
- Task Watchdog Timer (TWDT) with 10-second timeout and panic-on-timeout
- Heap guard (`heap_can_allocate`) before all runtime allocations ≥1KB
- RTSP client cap (3) and HTTP client cap (5) with explicit 503 rejection
- Reboot reason logging to NVS with persistent counter
- Bounded JPEG parser (`skipScanBytes`) replacing unbounded `while(true)`
- Template caching for web UI — render once, invalidate on config change
- IotWebConf + ESPAsyncWebServer coexistence (ports 8080 + 80)
- Integration test scripts for client cap enforcement
- Unit tests for `camera_map_quality` and `skipScanBytes`
- Non-blocking WiFi retry using millis()-based timing
- Frame buffer producer-consumer architecture via FreeRTOS queue

### Changed
- Build system: `-Ofast` → `-Os`, `VERBOSE` → `ERROR` log level, libraries pinned with `@=`
- `loop()` now contains only `vTaskDelete(NULL)` — all logic in FreeRTOS tasks
- DCW (downscale) disabled by default for sharper image
- Default JPEG quality improved from 12 to 8 (lower = higher quality)
- Micro-RTSP library vendored to `lib/` to preserve critical fixes
- Web server: synchronous `WebServer` replaced with `ESPAsyncWebServer` for all request handling
- IotWebConf scoped exclusively to WiFi provisioning (port 8080)

### Fixed
- **CPU starvation:** single-threaded `loop()` blocking on MJPEG stream → 5 dedicated FreeRTOS tasks
- **JPEG quality mismatch:** UI value 80 misinterpreted as "extremely low" by OV2640 driver → mapping layer
- **WiFi unreliable:** single `WiFi.begin()` failure → immediate AP mode → retry with backoff
- **Brownout disabled:** `WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0)` removed, detector always active
- **Unbounded parser:** `skipScanBytes()` infinite loop on malformed JPEG → bounded with SKIP_SCAN_MAX_ITER
- **No watchdog:** system hang until manual power cycle → TWDT with panic-on-timeout
- **Unbounded clients:** memory exhaustion from unlimited connections → hard caps with 503 rejection
- **No heap protection:** allocation without guard → `heap_can_allocate()` with 20% threshold

### Removed
- Synchronous `WebServer` class — replaced with `ESPAsyncWebServer`
- `-Ofast` optimization flag — replaced with `-Os` for timing determinism
- `CORE_DEBUG_LEVEL=VERBOSE` in committed config — replaced with `ERROR` for production

### Security
- No hardcoded WiFi credentials in source — all credentials in NVS via IotWebConf
- Bounded parsing on all external data (JPEG, RTSP, HTTP) — prevents buffer overrun

## [1.0.0] - Original Release

- Initial ESP32-CAM RTSP streaming implementation by Rene Zeldent
- Single-threaded Arduino loop architecture
- IotWebConf for WiFi provisioning
- Micro-RTSP library for RTSP streaming
- MJPEG HTTP streaming via synchronous WebServer
- OV2640 camera support with basic configuration

[1.0.0]: https://github.com/rzeldent/esp32cam-rtsp
