# Production Hardening Log

## Overview

This project started as an excellent ESP32-CAM RTSP streaming prototype by
[Rene Zeldent](https://github.com/rzeldent/esp32cam-rtsp). A systematic audit
identified 4 root-cause categories preventing field deployment. Over 8 sequential
hardening steps, each category was resolved against a defined engineering standard
([CLAUDE.md](../CLAUDE.md)). The result is firmware engineered for 72-hour
continuous operation with bounded resource consumption, watchdog protection, and
defensive parsing on every external-data path.

---

## Audit Summary (July 6, 2026)

The full audit is in [AUDIT_REPORT_V1.md](../AUDIT_REPORT_V1.md). Four root-cause
categories were identified, all confirmed from code evidence:

| # | Root Cause | Symptom | Severity |
|---|---|---|---|
| 1 | CPU / Task Starvation | Single `loop()` blocks RTSP when MJPEG client connects | Critical |
| 2 | JPEG Quality Mismatch | UI exposes 1–100, driver expects 0–63 (lower = higher quality) | Critical |
| 3 | WiFi No Retry | Single failure → immediate AP fallback, no debounce | High |
| 4 | Spontaneous Reboot | Brownout disabled, unbounded parsers, no TWDT, no heap guard, unbounded clients | Critical |

Each finding had a concrete code location, not a guess. The audit report documents
the exact file, line number, and evidence for every item — the hardening steps below
map 1:1 to those findings.

---

## Hardening Process

### Step 1: Build System & Global Config

**Problem:** `-Ofast` optimization produced timing-indeterministic code. `VERBOSE`
logging saturated the 115200 bps UART, becoming a serial bottleneck. Library
versions were unpinned — `platformio.ini` had zero version constraints, so a
`pio update` could silently pull breaking changes. The brownout detector was
explicitly disabled (`src/main.cpp:317`: `WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0)`),
masking power supply problems and replacing clean brownout resets with random
memory corruption crashes.

**Fix:**
- `-Ofast` → `-Os` — size-optimized, timing-deterministic
- `CORE_DEBUG_LEVEL` → `ARDUHAL_LOG_LEVEL_ERROR` for production
- All libraries pinned with exact versions (`@=x.y.z`)
- Created `include/config.h` — single source of truth for all compile-time constants:
  pins, buffer sizes, timeouts, threshold percentages
- Removed `WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0)` — brownout detector always active

**Files:** `platformio.ini`, `include/config.h`, `src/main.cpp`

**Standards reference:** CLAUDE.md §2 (Build System), §8 (Brownout detector must remain enabled)

---

### Step 2: FreeRTOS Task Decomposition

**Problem:** All logic lived inside Arduino's single `loop()` — a cooperative,
single-threaded execution model. The MJPEG HTTP handler (`handle_stream()`)
contained a `while (client.connected())` loop with zero `yield()`/`delay()` calls.
When one client viewed the MJPEG stream, the entire `loop()` never returned —
RTSP session handling, WiFi state machine, and IotWebConf housekeeping all stopped.
This was audit finding #1: CPU/task starvation.

**Fix:** Created 5 dedicated FreeRTOS tasks with pinned cores and explicit priorities:

| Task | Core | Priority | Stack | Responsibility |
|---|---|---|---|---|
| `task_camera` | 1 | 3 | 4096 | Frame capture cadence, hands buffers to consumers |
| `task_network` | 1 | 2 | 4096 | WiFi state machine, retry/backoff, IotWebConf `doLoop()` |
| `task_rtsp` | 0 | 2 | 6144 | RTSP session handling, frame dispatch to clients |
| `task_web_ui` | 0 | 1 | 4096 | HTTP handlers (async), template cache invalidation |
| `task_health` | 0 | 1 | 2048 | TWDT reset, heap monitoring, reboot-reason logging |

Camera capture gets the highest application priority — frame cadence is the
product's core function. WiFi/network work is pinned to core 1, camera/streaming
to core 0. `loop()` now contains only `vTaskDelete(NULL)`.

**Files:** `include/task_definitions.h`, `src/tasks/task_camera.cpp`, `src/tasks/task_network.cpp`, `src/tasks/task_rtsp.cpp`, `src/tasks/task_web_ui.cpp`, `src/tasks/task_health.cpp`, `src/main.cpp`

**Standards reference:** CLAUDE.md §5 (FreeRTOS Task Architecture), §3 (no business logic in `main.cpp`)

---

### Step 3: Camera Quality Mapping

**Problem:** The web UI exposed JPEG quality as 1–100 via IotWebConf
(`src/main.cpp:25`). This value was passed directly to `esp_camera_init`'s
`.jpeg_quality` field — which the OV2640 datasheet defines as **0–63, lower =
higher quality**. A user setting "80" (expecting high quality) got a driver
interpretation of "80" on a 0–63 scale: extremely low quality, effectively
unusable. This was audit finding #2.

Additionally, the default quality of 12 was unnecessarily conservative for
a PSRAM-equipped board, and DCW (digital downscale) was enabled by default,
softening the image.

**Fix:**
- Created `camera_map_quality(ui_value)` — maps UI 1–100 → driver 63–0:
  `driver_q = 63 - ((ui_q - 1) * 63 / 99)`
- UI = 100 → driver = 0 (highest quality)
- UI = 1 → driver = 63 (lowest quality)
- Default quality improved from 12 to 8
- DCW disabled by default for maximum sharpness
- `camera_set_quality()` is the sole entry point for runtime quality changes —
  it routes through the mapping layer so the UI→driver translation can never
  be bypassed

**Files:** `src/app/camera_manager.cpp`, `include/camera_manager.h`

**Standards reference:** CLAUDE.md §1 (OV2640 JPEG quality register range), §12 (never pass UI values directly to driver)

---

### Step 4: WiFi State Machine

**Problem:** The original code delegated all WiFi logic to IotWebConf's defaults:
a single `WiFi.begin()` call with no retry, immediate AP fallback on any failure,
and no disconnect debounce. On cold boot, `WIFI_PASSWORD = nullptr` forced AP
mode first (~30 seconds) before even attempting STA connection. A transient RF
glitch triggered a full reconnect cycle with no grace period. This was audit
finding #3.

**Fix:**
- **Exponential backoff:** 3 retry attempts at 1s, 2s, 4s delays before AP fallback.
  A single failed `WiFi.begin()` no longer triggers AP mode.
- **5-second disconnect debounce:** WiFi status must report disconnected
  continuously for 5 seconds before reconnect logic fires, preventing storms
  from transient RF glitches.
- **`WiFi.setAutoReconnect(true)`** at the ESP-IDF layer for hardware-level recovery.
- **`skipApStartup()`** — boots directly to STA if NVS-stored credentials exist,
  eliminating the mandatory ~30s AP delay.
- **Non-blocking retry** using `millis()`-based timing — no `delay()` calls that
  would starve other tasks or the TWDT.
- **IotWebConf scoped to provisioning only** on port 8080 — it owns the
  captive-portal and NVS config storage, nothing else.

**Files:** `src/app/network_manager.cpp`, `include/network_manager.h`, `src/tasks/task_network.cpp`

**Standards reference:** CLAUDE.md §7.2 (WiFi Connection Behavior), §7.1 (IotWebConf scoped to provisioning only)

---

### Step 5: Bounded Parsers

**Problem:** The function `skipScanBytes()` in the Micro-RTSP library contained
an unbounded `while(true)` loop chasing JPEG marker bytes through a buffer. The
original author left a comment: `// FIXME, check against length`. Malformed JPEG
data — from sensor glitches, low-light noise, or PSRAM corruption — would cause
the pointer to walk beyond the buffer boundary, triggering a Guru Meditation
Error (LoadProhibited) and spontaneous reboot. This was audit finding #4b.

The audit also flagged that no systematic check for unbounded loops over
external data existed anywhere in the codebase — this was the most dangerous
instance, but the risk class was broader.

**Fix:**
- Rewrote `skipScanBytes()` with two explicit bounds:
  - **Buffer length** — the pointer cannot advance past the buffer's known end
  - **Maximum iteration count** (1024) — a secondary hard stop if the buffer
    boundary is somehow not reached
- Returns `-1` on failure instead of crashing — the caller (`decodeJPEGfile()`)
  handles the error by aborting frame processing for that client
- Audited the entire codebase for unbounded loops over network/sensor data —
  none remain
- 12 unit tests cover valid JPEG data, boundary conditions, truncated buffers,
  and deliberately malformed input

**Files:** `lib/Micro-RTSP/src/CStreamer.cpp`, `test/test_parsers.cpp`

**Standards reference:** CLAUDE.md §4 (no unbounded loops parsing external/untrusted data), §8 (bounds-check all external-data parsing), §10.1 (pure logic unit tests)

---

### Step 6: Reliability Triad — TWDT + Heap Guard + Client Caps

**Problem:** Three independent reliability gaps, each capable of causing
unrecoverable failure:

1. **No Task Watchdog Timer** — if any task hung (blocked socket, infinite loop,
   deadlock), the system stayed hung until a manual power cycle. Zero TWDT
   configuration existed in the entire codebase.
2. **No heap guard** — allocations for RTSP client objects, `moustache_render()`
   output strings, and HTTP response buffers happened without checking available
   memory. Under load, allocation failure was silent → null pointer dereference
   → Guru Meditation → reboot.
3. **Unbounded client acceptance** — `rtsp_server.cpp` allocated a new
   `WiFiClient` on every `accept()` with no upper limit. An attacker (or buggy
   client retry loop) could exhaust heap by opening connections.

These were audit findings #4c, #4d, and #4e.

**Fix:**
- **TWDT:** Initialized with 10-second timeout and panic-on-timeout enabled.
  All 5 tasks are registered and reset the watchdog inside their main loops.
  A hung task triggers a documented panic, not a silent hang.
- **Heap guard:** `heap_can_allocate(size)` checks `ESP.getFreeHeap() > 20%`
  before any allocation ≥1KB. If below threshold, the operation is rejected
  with a logged reason. Wired before: RTSP client creation, template rendering,
  `new AsyncWebServer`, and any HTTP response buffer >1KB.
- **Client caps:** RTSP max 3 concurrent clients, HTTP max 5. Both reject
  excess connections with explicit status (HTTP 503 + `Retry-After` header;
  RTSP 503 equivalent). Atomic client counters with critical-section protection
  prevent race conditions.
- **Reboot reason logging:** `esp_reset_reason()` result + persistent counter
  written to NVS on every boot, making field failures diagnosable without a
  live serial session.

**Files:** `src/app/health_monitor.cpp`, `include/health_monitor.h`, `src/tasks/task_health.cpp`, `include/config.h`

**Standards reference:** CLAUDE.md §8 (TWDT mandatory, reboot reason logging), §6 (heap guard mandatory, client connection hard caps)

---

### Step 7: Web Server Stack Migration

**Problem:** The synchronous Arduino `WebServer` class handled all HTTP requests
inside the task context — every `handle_root()` call blocked until the full HTML
page (~4.5KB) was rendered and transmitted. `moustache_render()` parsed the
template and performed 50+ variable substitutions on every single `GET /`,
allocating a heap String each time. Under concurrent load, this created a
serial bottleneck that competed directly with streaming tasks. Template rendering
alone consumed measurable CPU and heap per request — even for a page whose content
hadn't changed.

**Fix:**
- **`WebServer` → `ESPAsyncWebServer` + `AsyncTCP`** for all request handling.
  Async callbacks run in the AsyncTCP task context, not blocking any application
  task. The synchronous `WebServer` class is completely removed from the codebase.
- **IotWebConf isolated to port 8080** (provisioning only), AsyncWebServer on
  port 80 (main UI). Both coexist without conflict.
- **Template cached as `static String`** — rendered once at startup and on config
  change, not per-request. Invalidation is wired to IotWebConf's config-save
  callback.
- **HTTP client cap (5) wired to all 4 route handlers** — root page, parameter
  form, stream endpoint, and status page all reject with 503 + `Retry-After`
  when at capacity.

**Files:** `src/app/web_ui_service.cpp`, `include/web_ui_service.h`, `src/tasks/task_web_ui.cpp`

**Standards reference:** CLAUDE.md §7.1 (Web Server Stack Decision — IotWebConf scoped to provisioning, ESPAsyncWebServer for all request handling, template caching mandated)

---

### Step 8: Final Audit & Cleanup

**Problem:** After the 7 major hardening steps, a compliance pass against
CLAUDE.md revealed 13 remaining items: magic numbers embedded in `.cpp` files,
a missing heap guard before `new AsyncWebServer`, two stub tasks (`task_rtsp`
and `task_camera`) that compiled but weren't wired to actual server logic, and
no integration test scripts for verifying client cap enforcement on hardware.

**Fix:**
- **All 13 magic numbers** extracted to `config.h` or `task_definitions.h` as
  named constants — no unexplained literals remain in logic files
- **`heap_can_allocate()`** added before `new AsyncWebServer` allocation
- **`task_rtsp`** wired to the RTSP server with a producer-consumer frame
  queue (FreeRTOS queue, `task_camera` → queue → `task_rtsp` → clients)
- **`task_camera`** wired as the sole frame producer, capturing at the
  configured cadence and dispatching via the queue
- **Integration test scripts** created: `test_rtsp_cap.py` verifies the 4th
  RTSP client is rejected, `test_http_cap.py` verifies the 6th HTTP client
  receives 503 + `Retry-After`

**Files:** All `src/tasks/`, `include/config.h`, `include/task_definitions.h`, `test/integration/test_rtsp_cap.py`, `test/integration/test_http_cap.py`

**Standards reference:** CLAUDE.md §4 (no magic numbers), §6 (heap guard mandatory), §10.3 (integration tests)

---

## Before & After

| Metric | Original (v1.0.0) | Hardened (v2.0.0) |
|---|---|---|
| Architecture | Single `loop()` | 5-task FreeRTOS, dual-core |
| JPEG Quality | Broken (1–100 → 0–63 mismatch) | Datasheet-compliant mapping |
| WiFi Resilience | No retry, instant AP fallback | 3-retry exponential backoff, 5s debounce |
| Watchdog | None | TWDT 10s with panic-on-timeout |
| Heap Safety | None | Guard before every allocation ≥1KB |
| Client Limit | Unlimited | RTSP 3 / HTTP 5 with 503 rejection |
| Parser Safety | Unbounded `while(true)` | Bounded with explicit limits |
| Web Server | Synchronous, blocking | Async, cached templates |
| Log Level | VERBOSE (serial bottleneck) | ERROR (production) |
| Build Optimization | `-Ofast` | `-Os` |
| Brownout Detector | Disabled | Enabled (always) |
| Dependencies | Unpinned | Exact versions (`@=x.y.z`) |
| Template Rendering | Every request | Once, invalidate on config change |
| Tests | None | 12 parser tests + 2 integration scripts |
| Reboot Diagnosis | Impossible without serial | NVS-persisted counter + reason code |

---

## Lessons Learned

### Never Disable the Brownout Detector

`WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0)` is common in ESP32-CAM tutorials
and forum posts. It masks power supply problems and replaces clean brownout
resets with random memory corruption crashes. The AI-Thinker ESP32-CAM's
onboard AMS1117 regulator is undersized for WiFi TX current spikes (~300mA
peak) — if brownouts occur, the fix is hardware (add a 1000µF bulk capacitor
on the 3.3V rail, or use a better supply), never firmware. Disabling the
detector doesn't fix the brownout; it just makes the resulting crash
undiagnosable.

### Always Bound External-Data Parsers

The `while(true)` in `skipScanBytes()` had the developer's own `FIXME` comment —
they knew it was dangerous but hadn't gotten to it yet. Malformed JPEG frames
happen in the real world: sensor glitches in low light, PSRAM bit flips from
cache-coherency bugs, truncated network payloads. An unbounded parser turns a
data problem into a system crash. Every parser over external data must have an
explicit upper bound on iteration count, buffer length, or both — no exceptions.

### Separate Framework from Logic

The original code mixed camera sensor configuration, WiFi state transitions,
and JPEG quality math directly with Arduino framework calls inside `loop()`.
Extracting pure logic into manager modules (`camera_manager`, `network_manager`,
`health_monitor`) served two purposes: it made the code testable on a host
machine (no hardware dependency for mapping functions and parsers), and it
prepared the architecture for ESP-IDF migration, where the business logic
modules become components that don't care which framework layer sits beneath them.

### Two Servers Are Better Than One Collapsed Stack

Keeping IotWebConf on port 8080 for provisioning while running
ESPAsyncWebServer on port 80 for the main UI was initially considered a
compromise. It turned out to be the correct architecture: provisioning
(write-once, rarely accessed) has fundamentally different requirements from
the main UI (read-heavy, polled frequently). Separating them eliminated an
entire class of contention bugs and made the async migration simpler — each
server could be migrated independently.

### AI Is a Force Multiplier, Not a Replacement

Claude Code via DeepSeek API accelerated code generation, audit scanning, and
test authoring, but every architectural decision — task priority assignments,
core pinning strategy, the 20% heap threshold, the choice to keep IotWebConf
rather than replace it — was made by the human engineer against the defined
standard. AI-generated code was reviewed against CLAUDE.md before commit;
nothing was merged unreviewed. The AI's value was in volume (scanning every
source file for unbounded loops, generating boilerplate FreeRTOS task wrappers,
producing test cases for edge conditions) — not in judgment.

---

## Validation Status

- [x] All 4 root-cause categories resolved (code-level verification against audit)
- [x] FreeRTOS task architecture matches CLAUDE.md §5 (5 tasks, correct cores/priorities)
- [x] Web server stack matches CLAUDE.md §7.1 (IotWebConf port 8080, AsyncWebServer port 80)
- [x] TWDT + brownout detector + heap guard active in all build targets
- [x] Client caps (RTSP 3, HTTP 5) enforced in code with atomic counters
- [x] Integration test scripts ready (`test/integration/test_rtsp_cap.py`, `test_http_cap.py`)
- [x] 12 unit tests for `skipScanBytes` covering valid, boundary, and malformed input
- [x] `camera_map_quality()` mapping verified against OV2640 datasheet range
- [ ] **72-hour soak test** with ≥1 RTSP client connected and web UI polled periodically
- [ ] **ESP-IDF migration** — blocked on all §11.1 exit criteria, including the soak test above

The unchecked items are the gate to CLAUDE.md §11's ESP-IDF migration trigger.
They require target hardware and extended runtime — they cannot be verified
from code alone.

---

*Hardening performed July 6–8, 2026. All changes are documented in detail in
[AUDIT_REPORT_V1.md](../AUDIT_REPORT_V1.md) (findings) and
[CHANGELOG.md](../CHANGELOG.md) (complete change inventory). Engineering
standards are in [CLAUDE.md](../CLAUDE.md).*
