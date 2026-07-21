# Embedded System Design — ESP32-CAM RTSP CCTV

**Target hardware:** AI-Thinker ESP32-CAM (ESP32 classic, OV2640, 4MB PSRAM, 4MB flash)
**Framework:** PlatformIO + Arduino (production), ESP-IDF migration gated behind §11 exit criteria
**Audience:** Embedded engineers evaluating this project for correctness, safety, and field-readiness
**Last updated:** 2026-07-08

---

## 1. Hardware Baseline

| Parameter | Value | Constraint / Implication |
|---|---|---|
| MCU | ESP32 (dual-core Xtensa LX6, 240 MHz) | Classic variant — no USB-OTG, no vector extensions. All decisions assume this silicon. |
| PSRAM | 4 MB, external, SPI-attached | Affected by **ESP32 classic PSRAM cache bug**. Mandatory build flag: `-mfix-esp32-psram-cache-issue` (`platformio.ini`, `config.h:237`). The hardware workaround is imperfect — when WiFi MAC and camera DMA access PSRAM concurrently, cache-coherency violations can corrupt data. Core-affinity pinning (§2) mitigates this by separating the WiFi stack (core 1) from frame-buffer consumers (core 0). |
| Flash | 4 MB, SPI | Single-app partition currently. Dual-app (ota_0/ota_1) layout required before OTA is introduced (`CLAUDE.md` §7). Partition ceiling: 80% of defined partition (`config.h`). |
| Camera | OV2640, SCCB on I2C_NUM_0 | JPEG quality register range is **0–63, lower = higher quality** (`config.h:63-64`). The UI exposes 1–100 (higher = better). `camera_map_quality()` in `camera_manager.cpp` performs the inversion: `driver = 63 * (100 - ui) / 100`. Never pass UI values directly to the driver (`CLAUDE.md` §12). |
| Voltage regulator | AMS1117 3.3V (onboard) | **Undersized for WiFi TX spikes** (~300 mA). The regulator is a known weak point on this board. Firmware must not paper over it by disabling the brownout detector — the fix is a bulk capacitor on the 3.3V rail (`CLAUDE.md` §8, `AUDIT_REPORT_V1.md` §4, Candidate A). |
| Power | Mains (prototype) | Indoor, fixed location. No battery or deep-sleep constraints. Operating temperature range documented in `config.h` as a compile-time constant. |

### Known Hardware Issues

1. **PSRAM cache coherency (ESP32 silicon erratum):** Simultaneous access from CPU core 0 and WiFi DMA (core 1) can produce corrupted reads. The `-mfix-esp32-psram-cache-issue` flag inserts cache maintenance operations but carries a ~15–20% throughput penalty on PSRAM-heavy paths. Core-affinity pinning (§2) reduces contention probability but does not eliminate it.

2. **AMS1117 brownout under WiFi TX:** The onboard regulator cannot sustain the ~300 mA TX current spike plus camera LED flash plus frame-buffer PSRAM access. Without the brownout detector, this manifests as silent register corruption rather than a clean reset, making failures non-deterministic and undiagnosable.

3. **No power-down or reset pins on the camera sensor:** `CAM_PIN_PWDN = -1`, `CAM_PIN_RESET = -1` (`config.h:48-49`). The sensor cannot be hardware-cycled independently of the MCU. A camera init failure requires an MCU-level reboot.

---

## 2. System Architecture

### 2.1 Task Model

The system runs five FreeRTOS tasks. No business logic remains in Arduino's cooperative `loop()` — it is a thin orchestrator that creates tasks, initializes managers, and returns (`main.cpp:48`, `CLAUDE.md` §3).

| Task | Core | Priority | Stack (words) | Loop period | Responsibility |
|---|---|---|---|---|---|
| `task_camera` | 1 | 3 | 4096 | 40 ms | Frame capture at ~25 fps, enqueues `camera_fb_t*` to RTSP consumer |
| `task_network` | 1 | 2 | 4096 | 1000 ms | WiFi state machine, IotWebConf `doLoop()`, retry/backoff, MQTT (future) |
| `task_rtsp` | 0 | 2 | 6144 | 40 ms | RTSP session accept/teardown, frame dequeue from camera, broadcast to clients |
| `task_web_ui` | 0 | 1 | 4096 | 2000 ms | Template cache invalidation, status JSON rebuild, IotWebConf housekeeping |
| `task_health` | 0 | 1 | 2048 | 5000 ms | TWDT reset, heap monitoring, client-count telemetry, reboot-reason logging |

All task handles, priorities, stack sizes, and loop intervals are declared in a single file: `include/task_definitions.h` (`CLAUDE.md` §3, §5).

### 2.2 Core Affinity Rationale

- **Core 1 (PRO CPU):** WiFi MAC, LWIP stack, camera capture, network state machine. The Espressif SDK runs the WiFi task on core 0 by default; we pin network-adjacent work to core 1 to reduce cross-core cache-invalidation traffic on the PSRAM bus.
- **Core 0 (APP CPU):** RTSP streaming, web UI, health monitor. Frame-buffer readout (RTSP) and HTTP handlers do not contend with the WiFi MAC on the same core, reducing PSRAM cache-bug exposure.

Camera capture gets the highest application priority (3) on its core — a stalled UI must never stall the frame cadence (`CLAUDE.md` §5).

### 2.3 Inter-Task Communication

- **Camera → RTSP:** FreeRTOS queue (`camera_fb_t*`, depth = `CAMERA_FB_COUNT = 2`). Single-producer (task_camera), single-consumer (task_rtsp). `config.h:145-156` enforces that queue depth matches the frame-buffer count via a `static_assert`.
- **Network → Web UI:** `WiFiStatus` struct snapshot via `network_get_status()` (`network_manager.h:31-37`). Thread-safe read — no mutex, the struct is small enough for atomic copy on Xtensa.
- **Health → All:** TWDT registration array (`task_definitions.h:82`). Each task's handle is registered; `task_health` calls `esp_task_wdt_reset()` on behalf of all registered tasks every 5 seconds.
- **Client caps (RTSP/HTTP):** Atomic counters via `rtsp_client_accept()`/`http_client_accept()` in `health_monitor.h`. State is read from any task via `health_get_rtsp_client_count()` / `health_get_http_client_count()`.

### 2.4 Web Server Stack

The stack is fixed by `CLAUDE.md` §7.1:

- **IotWebConf** — scoped strictly to WiFi provisioning (captive portal, NVS config storage). Its internal synchronous `WebServer` runs on port 8080 (`config.h:195`).
- **ESPAsyncWebServer + AsyncTCP** — all application HTTP request handling on port 80 (`config.h:196`). Async callbacks run in the AsyncTCP task context, not in `task_web_ui` itself.
- **`task_web_ui`** is a supervisory task — it rebuilds cached status JSON, invalidates the template cache when parameters change, and calls `iotWebConf.doLoop()` for non-streaming housekeeping.

This replaces the original architecture where all HTTP handling was synchronous and shared the `loop()` context with RTSP streaming, causing mutual starvation (`AUDIT_REPORT_V1.md` §1, Finding 1).

---

## 3. Memory Budget

### 3.1 DRAM (Internal ~320 KB total)

| Consumer | Typical Usage | Constraint |
|---|---|---|
| WiFi stack + LWIP | ~60 KB | Espressif baseline, non-negotiable |
| RTSP static buffers (CurRequest, RecvBuf, Response×7, RtpBuf) | ~24 KB BSS | From `CRtspSession.cpp` and `CStreamer.cpp` (`AUDIT_REPORT_V1.md` §4, Candidate C) |
| FreeRTOS task stacks (5 tasks) | ~24 KB | Sum of `STACK_*` words × 4 bytes (`task_definitions.h:52-56`) |
| IotWebConf + web server | ~16 KB | Includes AsyncTCP internal buffers |
| Application overhead | ~16 KB | NVS, heap allocator metadata, Arduino init |

**Heap guard threshold:** Before any allocation ≥1 KB, `heap_can_allocate(size)` in `health_monitor.h` verifies that `free_heap >= size AND free_heap >= 20% of total heap` (`config.h:119`). If either check fails, the operation is rejected with an explicit error (HTTP 503 + `Retry-After` header; RTSP equivalent error response).

### 3.2 PSRAM (4 MB)

PSRAM is consumed primarily by camera frame buffers. The driver allocates `fb_count = CAMERA_FB_COUNT = 2` buffers at SVGA (800×600 JPEG). At typical JPEG quality 8, each buffer is ~50–80 KB. Total PSRAM steady-state: ~160 KB of 4096 KB (~4%).

**Ceiling:** 85% at any steady-state runtime moment (`CLAUDE.md` §6). Monitored every 5 seconds by `task_health`.

### 3.3 Flash (4 MB)

**Ceiling:** 80% of the defined partition (`CLAUDE.md` §6). The dual-app OTA layout (ota_0/ota_1) will halve the available space per app image, requiring re-profiling before OTA is enabled.

### 3.4 Static Buffer Justification

Each static buffer >1 KB must carry a comment stating why it is not pooled or streamed, and its contribution to the DRAM budget must be tracked in `config.h` as a named constant with a running total (`CLAUDE.md` §6).

---

## 4. Reliability Strategy

### 4.1 Brownout Detector

**Status: always enabled** (`main.cpp:50-52`). The audit found `WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0)` at the original `main.cpp:317` (`AUDIT_REPORT_V1.md` §4, Candidate A). That line has been removed. If brownout resets occur in the field, the fix is hardware (bulk capacitor), not firmware (`CLAUDE.md` §12).

### 4.2 Task Watchdog Timer (TWDT)

Configured with **10-second timeout, panic-on-timeout enabled** (`config.h:165`, `CLAUDE.md` §8). All five application tasks are registered via `health_init()` (`task_definitions.h:82`). `task_health` resets TWDT for all registered tasks every 5 seconds. A task that cannot yield within 10 seconds triggers a panic — this is a bug to fix, not a reason to disable TWDT (`CLAUDE.md` §8).

### 4.3 Heap Guard

`heap_can_allocate(size)` in `health_monitor.cpp` (`health_monitor.h:72`) is the single gate for all runtime allocations ≥1 KB. It enforces:
1. The allocation must physically fit in the current free heap.
2. After allocation, at least 20% of total heap must remain free (`HEAP_GUARD_THRESHOLD = 0.2`, `config.h:119`).

Rejection is logged with the requested size and current free heap. This gate is called from `camera_init()`, `camera_fb_get()`, RTSP client accept, and web UI handler entry points.

### 4.4 Client Connection Hard Caps

| Protocol | Cap | Constant | Rejection Response |
|---|---|---|---|
| RTSP | 3 concurrent sessions | `MAX_RTSP_CLIENTS = 3` (`config.h:109`) | RTSP error response (equivalent to HTTP 503) |
| HTTP | 5 concurrent sessions | `MAX_HTTP_CLIENTS = 5` (`config.h:110`) | HTTP 503 + `Retry-After` header |

Enforced by `rtsp_client_accept()` / `http_client_accept()` in `health_monitor.h`. Connections are never silently dropped (`CLAUDE.md` §6).

### 4.5 Bounded Parsing

All parsing loops over external/untrusted data (JPEG streams, RTP, HTTP input, RTSP requests) carry an explicit iteration bound. The `skipScanBytes()` function that the audit identified as an unbounded `while(true)` pointer chase (`CStreamer.cpp:271-281`, `AUDIT_REPORT_V1.md` §4, Candidate B) has been bounded: `SKIP_SCAN_MAX_ITER = 1024` (`config.h:229`). Loops that exceed this bound return a defined failure code — no silent hang, no undefined behavior.

### 4.6 Reboot Reason Logging

On every boot, `health_init()` calls `log_reboot_reason()` (`health_monitor.h:81`), which reads `esp_reset_reason()`, stores the reason code and increments a boot counter in NVS under the `"health"` namespace, and prints the reason to Serial. This makes field failures diagnosable without a live serial session (`CLAUDE.md` §8).

### 4.7 WiFi Connection State Machine

`network_manager.cpp` implements a state machine (`WiFiState` enum, `network_manager.h:20-26`) with:

- **Retry with exponential backoff:** 3 attempts at 1s / 2s / 4s delays before AP-mode fallback (`config.h:180-183`).
- **Disconnect debounce:** 5 seconds of continuous `WL_DISCONNECTED` before triggering reconnect logic (`config.h:184`).
- **`WiFi.setAutoReconnect(true)`** at the ESP-IDF layer.
- **`skipApStartup()`** when stored credentials exist — avoids the ~30-second AP-mode delay on cold boot.
- **User-visible status** exposed via `/status` JSON and the web UI: current state, RSSI, retry count, last error string, uptime.

---

## 5. Failure Mode Analysis

| Failure | Detection | Response | Recovery |
|---|---|---|---|
| **Camera init fails** (I2C glitch, power-on race) | `camera_init()` returns `esp_err_t` | Retry up to `CAMERA_INIT_RETRY_COUNT = 3` times at 500 ms intervals (`config.h:76-77`). On final failure, log the error, set `camera_init_result != ESP_OK`. | System boots without camera. `task_camera` detects the failed init and does not attempt frame capture. `task_health` logs the degraded state. No panic. Manual intervention required (power cycle). |
| **WiFi disconnects** (RF interference, AP reboot) | `WiFi.status() != WL_CONNECTED` for ≥5 continuous seconds | Debounce timer starts. After 5 seconds, transition to `STA_CONNECTING` state. Execute retry sequence (3 attempts, backoff). | If retries succeed: reconnect, zero retry counter. If all retries fail: transition to `AP_MODE`, start captive portal for re-provisioning. All RTSP clients are disconnected gracefully. |
| **Heap exhausts** (allocation surge, fragmentation) | `heap_can_allocate(size)` returns false | Reject the offending operation. HTTP: 503 + `Retry-After`. RTSP: equivalent error. Camera: skip frame, return nullptr. | No crash. The system degrades: new clients are rejected, frames may be dropped, but existing tasks continue running. `task_health` logs the rejection. Recovery when existing allocations are freed (client disconnect, frame buffer return). |
| **4th RTSP client connects** | `rtsp_client_accept()` returns false | Reject with explicit error response. Cap counter unchanged. | Existing 3 sessions unaffected. New client receives documented rejection — retry guidance implicit in the protocol error. |
| **6th HTTP client connects** | `http_client_accept()` returns false | HTTP 503 + `Retry-After` header. | Existing 5 sessions unaffected. |
| **Malformed JPEG received** (sensor glitch, PSRAM corruption) | `parseMoreData()` exit with error after `SKIP_SCAN_MAX_ITER` exceeded | Frame discarded. `esp_camera_fb_return()` called. | Next frame captured normally. No crash, no hang. |
| **Task hangs** (deadlock, infinite loop) | TWDT timeout (10 s) | Hardware panic via TWDT. Reboot reason logged to NVS. | Clean reboot. `log_reboot_reason()` records the panic cause on next boot. |
| **Brownout** (regulator sag under WiFi TX load) | Brownout detector (hardware, always enabled) | Hardware reset at ~2.7V. | Clean reset (vs. silent corruption if the detector were disabled). Reboot reason = `ESP_RST_BROWNOUT` logged to NVS. If brownouts are frequent, the hardware fix (bulk capacitor) is indicated. |

---

## 6. Testability

### 6.1 Host-Testable (Pure Logic)

Functions with no ESP-IDF/Arduino dependency are testable via `pio test -e native` (`CLAUDE.md` §10.1):

- `camera_map_quality()` — JPEG quality range mapping (UI 1–100 → driver 63–0). Test vectors documented in `camera_manager.h:32-35`.
- `heap_can_allocate()` — threshold comparison logic.
- `rtsp_client_accept()` / `http_client_accept()` — cap enforcement and counter management.
- `network_get_status()` — struct snapshot consistency.
- Any parser or state transition that operates on scalar/buffer inputs.

### 6.2 Module Contract Tests (Mocked HAL)

Every function returning `esp_err_t` must have at least one error-path test (`CLAUDE.md` §10.2):
- `camera_init()` failure path (heap below guard threshold).
- `camera_fb_get()` returns nullptr when heap is exhausted.
- Client cap rejection returns correct error code.
- TWDT registration failure is handled without panic.

### 6.3 Integration Tests (Target Hardware)

Scripted scenarios in `test/integration/` (`CLAUDE.md` §10.3):
- **RTSP 4th client → 503:** Python script opens 4 concurrent RTSP connections, verifies the 4th is rejected.
- **HTTP 6th client → 503 + Retry-After:** HTTP load test verifies cap enforcement.
- **WiFi disconnect → debounce 5s → reconnect within 15s:** Kill AP, observe debounce, verify reconnection timeline.
- **Camera init failure → degraded boot:** Simulate by disconnecting the camera ribbon, verify system boots without panic and `task_health` logs the failure.

### 6.4 Soak Test (72 Hours)

The production-readiness gate (`CLAUDE.md` §11.1): 72-hour continuous run with ≥1 RTSP client connected and web UI polled periodically. Metrics logged every 60 seconds (`config.h:171`): free heap, PSRAM usage, client count, stack high watermarks, reboot counter.

Pass criteria: zero unexplained reboots, PSRAM ≤85%, heap degradation <5% over 72 hours, all client caps enforced throughout.

### 6.5 Code-Level Testability

- Functions that interact with hardware expose test seams: `#ifdef TEST` blocks or dependency injection.
- No function exceeds 40 lines unless it is a pure initializer with no branching (`CLAUDE.md` §10.5).
- All test assertions carry a descriptive message string.

---

## 7. Known Limitations

The following are explicitly out of scope for the current production-target firmware. Each is documented so evaluators understand that the omission is deliberate, not an oversight.

| Feature | Status | Precondition for Addition |
|---|---|---|
| **OTA firmware updates** | Not implemented | Requires signed-payload verification + dual-app partition table (ota_0/ota_1) with rollback support (`CLAUDE.md` §7). |
| **MQTT telemetry** | Stub only (`mqtt_service.{h,cpp}` placeholder) | Requires a task prompt requesting it. When implemented: QoS ≥1 for state-changing messages, explicit message schemas, reconnect backoff mirroring WiFi policy. |
| **Multi-device / fleet management** | Not implemented | Explicitly deferred to post-ESP-IDF-migration era (`CLAUDE.md` §9, §11.3). |
| **Battery / deep-sleep operation** | Not applicable | The device is designed for mains power, indoor, fixed location. Power assumptions are documented in `config.h`. |
| **TLS/SSL for RTSP** | Not implemented | RTSP runs on port 554, unencrypted. TLS termination would require a proxy or a separate secure-transport RTSPS port. |
| **Hardware watchdog (RTC WDT)** | Not used | We rely on TWDT (software) and brownout detector (hardware). The RTC WDT is available on ESP32 but not configured — it can be added if TWDT proves insufficient for a specific failure mode. |
| **External storage (SD card)** | Not implemented | The AI-Thinker board has a microSD slot. It is not used in the current firmware. Frame recording/playback is out of scope. |
| **Audio streaming** | Not implemented | OV2640 does not have a microphone. The board has no audio input path. |

---

## 8. Key Files Reference

| File | Role |
|---|---|
| `include/config.h` | All compile-time constants: pins, buffer sizes, timeouts, thresholds, caps, retry counts. Single source of truth. |
| `include/settings.h` | Runtime-configurable defaults (IotWebConf-backed): WiFi AP name, RTSP port, frame size, JPEG quality, camera tuning params. |
| `include/task_definitions.h` | Task names, core affinities, priorities, stack sizes, loop intervals, TWDT registration count. |
| `include/camera_manager.h` | Camera init API, quality mapping (`camera_map_quality()`), heap-guarded frame buffer acquire/release. |
| `include/network_manager.h` | WiFi state machine API, `WiFiState` enum, `WiFiStatus` struct, IotWebConf integration. |
| `include/health_monitor.h` | TWDT init, client caps, heap guard, reboot reason logging, status queries. |
| `include/web_ui_service.h` | AsyncWebServer initialization, template cache, `/status` JSON endpoint. |
| `src/main.cpp` | Orchestration: brownout-detector-on guard comment, task creation, manager init. |
| `src/tasks/task_*.cpp` | One file per FreeRTOS task entry function. |
| `test/test_parsers.cpp` | Host-side unit tests for pure-logic functions (quality mapping, parsers). |
| `test/integration/` | Python/shell scripts for targeted integration scenarios. |
| `CLAUDE.md` | Project engineering standard — the authority for all rules referenced in this document. |
| `AUDIT_REPORT_V1.md` | Initial audit (2026-07-06) — root-cause analysis of the four critical symptoms, with line-level evidence and fix recommendations. |

---

*This document is part of the ESP32-CAM RTSP CCTV production-hardened fork. It is a companion to `CLAUDE.md` (the engineering standard) and `AUDIT_REPORT_V1.md` (the initial audit). Together, the three documents form the complete design rationale for this firmware.*
