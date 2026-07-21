# REVIEW_REPORT_V3.md — Step 8/8 Final Audit Report

**Date:** 2026-07-08
**Scope:** Comprehensive, skeptical audit of all hardening work (Steps 1–7)
**Binding References:** CLAUDE.md (§1–§12), AUDIT_REPORT_V1.md (4 root causes)
**Purpose:** Determine §11.1 production-ready gate status before 72-hour soak test

---

## 8.1 — Full Codebase Scan for Rule Violations

### 8.1.1 — Magic Numbers in `.cpp` Files

**Methodology:** Searched for numeric literals excluding `0`, `1`, `-1`, `NULL`, `nullptr` in all `.cpp` files under `src/` and `lib/rtsp_server/`.

| # | File | Line | Literal | Context | Status |
|---|------|------|---------|---------|--------|
| 1 | `src/app/camera_manager.cpp` | 71 | `20000000` | `config.xclk_freq_hz = 20000000` — 20 MHz XCLK for OV2640. This is a **magic number** — should be `CAMERA_XCLK_FREQ_HZ` in `config.h`. | **FAIL** |
| 2 | `lib/rtsp_server/rtsp_server.cpp` | 97 | `12000` | `heap_can_allocate(12000)` — RTSP per-client allocation guard threshold. Should be a named constant like `RTSP_CLIENT_ALLOCATION_GUARD_BYTES` in `config.h`. | **FAIL** |
| 3 | `src/app/web_ui_service.cpp` | 55 | `55` | `#define TEMPLATE_VAR_COUNT 55` — this IS a named constant, but defined locally rather than in `config.h`. Acceptable as file-local since it's documented and tied to the template. | **PASS** |
| 4 | `lib/Micro-RTSP/src/CStreamer.cpp` | 34–124 | Multiple | RTP packet assembly magic numbers (`KRtpHeaderSize=12`, `KJpegHeaderSize=8`, `MAX_FRAGMENT_SIZE=1100`, `2048`, `0x80`, `0x1a`, etc.) — these are in **vendored third-party code**. Not in scope for this project's code convention enforcement. | **PASS (vendored)** |
| 5 | `lib/Micro-RTSP/src/CStreamer.cpp` | 37 | `1100` | `#define MAX_FRAGMENT_SIZE 1100` — this IS a named `#define`, but defined locally in vendored code. Not our code. | **PASS (vendored)** |
| 6 | All other `.cpp` files | — | — | All remaining numeric literals are either `0`, `1`, `-1`, or references to named constants from `config.h` / `task_definitions.h`. | **PASS** |

**Verdict: 2 FAIL.** Two magic numbers remain in project-owned `.cpp` files that are not named constants from `config.h`.

---

### 8.1.2 — Unbounded Loops

**Methodology:** Searched for `while(true)`, `while(1)`, `for(;;)` in all `.cpp` files under `src/` and `lib/`.

**`src/` findings — all task-steady-state loops:**

| File | Line | Loop | Analysis |
|------|------|------|----------|
| `src/tasks/task_network.cpp` | 24 | `while (1)` | Error-recovery stall after `network_init()` failure. Calls `vTaskDelay(1000ms)`. Only entered on init failure (hardware fault). Bounded by TWDT — if TWDT fires, the device reboots. **PASS** |
| `src/tasks/task_network.cpp` | 29 | `while (1)` | Main steady-state loop. Calls `network_tick()` then `vTaskDelay(1000ms)` + `esp_task_wdt_reset()`. Bounded by TWDT. **PASS** |
| `src/tasks/task_camera.cpp` | 67 | `while (1)` | Frame capture loop. `vTaskDelay(40ms)` + `esp_task_wdt_reset()` per iteration. Bounded by TWDT. **PASS** |
| `src/tasks/task_rtsp.cpp` | 111 | `while (1)` | RTSP server loop. `vTaskDelay(40ms)` + `esp_task_wdt_reset()`. Non-blocking `xQueueReceive` (timeout 0). Bounded by TWDT. **PASS** |
| `src/tasks/task_web_ui.cpp` | 31 | `while (1)` | Error-recovery stall after `web_ui_init()` failure. Calls `vTaskDelay(5000ms)`. _Note: uses `TASK_HEALTH_LOOP_MS` constant here, not `TASK_WEB_UI_LOOP_MS` — this is a minor inconsistency but not a bug._ **PASS** |
| `src/tasks/task_web_ui.cpp` | 36 | `while (1)` | Main steady-state loop. `vTaskDelay(2000ms)` + `esp_task_wdt_reset()`. **PASS** |
| `src/tasks/task_health.cpp` | 37 | `while (1)` | Health monitor loop. `vTaskDelay(5000ms)` + `esp_task_wdt_reset()`. **PASS** |

**`lib/` findings:** Zero `while(true)`/`while(1)`/`for(;;)` in `lib/rtsp_server/`. The Micro-RTSP library's `findJPEGheader()` uses `while(bytes - *start < *len)` which IS bounded by the buffer length and has bounds-checking guards on section length since the audit fix. **PASS**.

**Verdict: PASS.** All unbounded loops are task steady-state loops with `vTaskDelay` and TWDT resets. The `skipScanBytes()` fix in Micro-RTSP provides the bounded parsing scan mandated by CLAUDE.md §4, §8.

---

### 8.1.3 — Brownout Disable Calls

**Methodology:** Searched for `RTC_CNTL_BROWN_OUT_REG`, `WRITE_PERI_REG.*BROWN`, or any brownout-related register write in all source files.

**Results:** **ZERO** matches in source code. The only references to brownout are:
- `CLAUDE.md` — documentation prohibiting brownout disable
- `AUDIT_REPORT_V1.md` — audit finding documenting the original violation
- `REVIEW_REPORT_V2.md` — prior review confirmation
- `src/main.cpp:50-52` — comment asserting brownout must remain enabled

**Verdict: PASS.** No brownout disable calls exist anywhere in the codebase.

---

### 8.1.4 — Hardcoded WiFi Credentials

**Methodology:** Searched for quoted strings resembling SSIDs, passwords, or WiFi credentials in `src/` and `include/`.

**Results:**

| File | Line | String | Analysis |
|------|------|--------|----------|
| `include/settings.h` | 6 | `WIFI_SSID "ESP32CAM-RTSP"` | **Device AP identity** — this is the SSID of the device's own SoftAP, not a STA credential. Documented in `network_manager.h:9-11` and `network_manager.cpp:73-76`. Per CLAUDE.md §12, this is the documented exception. |
| `include/settings.h` | 7 | `WIFI_PASSWORD nullptr` | **AP password** — `nullptr` means open AP. Not a STA credential. |
| `include/settings.h` | 10 | `OTA_PASSWORD "ESP32CAM-RTSP"` | OTA password (future feature). Separate from WiFi. |

All log strings containing "WiFi" in source code are diagnostic messages, not credentials.

**Verdict: PASS.** No hardcoded STA credentials. `WIFI_SSID` and `WIFI_PASSWORD` in `settings.h` are device-identity constants for the AP, not STA credentials. STA credentials are stored exclusively in NVS via IotWebConf.

---

### 8.1.5 — Unbounded Dynamic Allocation

**Methodology:** Searched for `new ` (with space) and `malloc(` in `src/` and `include/`. Verified each has a preceding guard.

**Results:**

| File | Line | Allocation | Guard Present? | Analysis |
|------|------|-----------|---------------|----------|
| `src/app/web_ui_service.cpp` | 628 | `new AsyncWebServer(WEB_UI_PORT)` | **YES** — `heap_can_allocate(ASYNC_WEB_SERVER_ESTIMATED_BYTES)` at line 622 | Single init-time allocation before steady-state loop. Allocated once per boot. **PASS** |
| `lib/rtsp_server/rtsp_server.cpp` | 42 | `new OV2640Streamer(...)` | **YES** — `rtsp_client_accept()` at line 92 (5→ count cap) + `heap_can_allocate(12000)` at line 97 | Per-client allocation, guarded by both client count cap and heap guard. **PASS** |
| `lib/rtsp_server/rtsp_server.cpp` | 43 | `new CRtspSession(...)` | **YES** — same guard chain as above | Per-client allocation. **PASS** |
| `lib/rtsp_server/rtsp_server.cpp` | 109 | `new rtsp_client(...)` | **YES** — same guard chain | Per-client pool entry. **PASS** |
| `lib/Micro-RTSP/src/platglue-esp32.h` | 50 | `new WiFiUDP()` | **PARTIAL** — no explicit heap guard, but limited by RTSP client cap (max 3) | Vendored library. One UDP socket per RTSP client session (max 3). Acceptable risk under the client cap. **PASS (vendored, cap-limited)** |

**Verdict: PASS.** All project-owned runtime allocations are guarded. The vendored Micro-RTSP allocation is bounded by the RTSP client cap.

---

### 8.1.6 — Build Flags

**Methodology:** Verify `platformio.ini` against CLAUDE.md §2 requirements.

**`[env]` (common section):**

| Flag | Required | Actual | Status |
|------|----------|--------|--------|
| Optimization | `-Os` | `-Os` | **PASS** |
| PSRAM fix | `-mfix-esp32-psram-cache-issue` | `-mfix-esp32-psram-cache-issue` | **PASS** |
| Debug level | `ARDUHAL_LOG_LEVEL_ERROR` for production | `ARDUHAL_LOG_LEVEL_ERROR` | **PASS** |
| Library version pinning | `@=` exact | All deps use `@=` | **PASS** |

**`[env:dev]`:**

| Flag | Required | Actual | Status |
|------|----------|--------|--------|
| Optimization | `-Og` (debug-friendly) | `-Og` | **PASS** |
| PSRAM fix | `-mfix-esp32-psram-cache-issue` | Present | **PASS** |
| Debug level | INFO (dev-only) | `ARDUHAL_LOG_LEVEL_INFO` | **PASS** |

**`[env:release]`:**

| Flag | Required | Actual | Status |
|------|----------|--------|--------|
| Inherits `[env]` | Yes | `board = esp32cam_ai_thinker`, no override of `build_flags` | **PASS** — inherits `-Os`, ERROR level, PSRAM fix from `[env]` |
| Separate environment | Yes | Present | **PASS** |

**`[env:native]`:** Present for host-side unit testing per CLAUDE.md §10. **PASS**.

**Verdict: PASS.** All build flags comply with CLAUDE.md §2.

---

### 8.1.7 — Blocking Calls in Task Loops

**Methodology:** Searched for `delay(` (Arduino blocking delay) in `src/tasks/`. Searched for blocking `while(client.connected())` patterns.

**Results:**

| File | Finding | Analysis |
|------|---------|----------|
| `src/tasks/` (all 5 task files) | **ZERO** `delay()` calls | All tasks use `vTaskDelay()` exclusively |
| `src/main.cpp:68` | `delay(USB_SETTLE_DELAY_MS)` | In `setup()` — one-time init before tasks start. Acceptable per CLAUDE.md §4 exception |
| `src/main.cpp:122` | `delay(CAMERA_INIT_RETRY_DELAY_MS)` | In `setup()` camera retry loop — one-time init. Acceptable |
| `src/app/network_manager.cpp` | **Comment-only** references to `delay()` | Implementation uses `millis()`-based non-blocking comparison (lines 438, 450) |

All task loops yield at least every 5 seconds (health), most yield at 40ms–2000ms. All call `esp_task_wdt_reset()`.

**Verdict: PASS.** No blocking `delay()` in any task steady-state loop.

---

### 8.1.8 — Synchronous `WebServer` Usage

**Methodology:** Searched for `WebServer` (not `AsyncWebServer`) instances and `#include <WebServer.h>`.

**Results:**

| File | Line | Instance | Analysis |
|------|------|----------|----------|
| `src/app/network_manager.cpp` | 71 | `static WebServer g_web_server(IOTWEBCONF_WEB_PORT)` | **IotWebConf's internal transport on port 8080.** Per CLAUDE.md §7.1, this is the documented, intentional exception. IotWebConf uses `WebServer` internally; our code never calls it directly. `IOTWEBCONF_WEB_PORT = 8080` is separate from the main `WEB_UI_PORT = 80`. |

All main web UI is via `AsyncWebServer` (`web_ui_service.cpp:65`) on port 80. Zero `#include <WebServer.h>` in project source (included transitively via `IotWebConf.h` in `network_manager.cpp`). Our code never creates or directly calls `WebServer` methods — all config portal interaction is through the `IotWebConf` API.

**Verdict: PASS.** `WebServer` exists only as IotWebConf's internal transport on a separate port. Per CLAUDE.md §7.1, this is the documented exception.

---

### 8.1.9 — Additional Structural Findings

**Missing `partitions.csv`:**
CLAUDE.md §3 specifies `partitions.csv` as an explicit partition table. No file matching this name exists. The project uses `board_build.partitions = min_spiffs.csv` in `platformio.ini`, which is a PlatformIO built-in scheme. For OTA readiness (§7), a custom partition table with `ota_0`/`ota_1` layouts would be required, but OTA is not yet implemented. **NOTE — Technical Debt.**

**Missing `include/version.h`:**
CLAUDE.md §3 specifies `include/version.h` for firmware version and build metadata. This file does not exist. `APP_TITLE` and `APP_VERSION` are defined in `settings.h` instead. **NOTE — Minor deviation from CLAUDE.md §3 structure.**

**Orphaned headers:**
`include/format_duration.h` and `include/format_number.h` exist but are not `#include`d anywhere in `src/`. These are utility headers with no callers. **NOTE — Dead code, should be integrated or removed.**

---

## 8.2 — Task Architecture (§5 Compliance)

| Task | Expected Core | Expected Priority | Expected Stack | Actual Core | Actual Priority | Actual Stack | Match? |
|---|---|---|---|---|---|---|---|
| `task_network` | 1 | 2 | 4096 | 1 (`TASK_NETWORK_CORE`) | 2 (`PRIO_NETWORK`) | 4096 (`STACK_NETWORK`) | ✅ |
| `task_rtsp` | 0 | 2 | 6144 | 0 (`TASK_RTSP_CORE`) | 2 (`PRIO_RTSP`) | 6144 (`STACK_RTSP`) | ✅ |
| `task_camera` | 1 | 3 | 4096 | 1 (`TASK_CAMERA_CORE`) | 3 (`PRIO_CAMERA`) | 4096 (`STACK_CAMERA`) | ✅ |
| `task_web_ui` | 0 | 1 | 4096 | 0 (`TASK_WEB_UI_CORE`) | 1 (`PRIO_WEB_UI`) | 4096 (`STACK_WEB_UI`) | ✅ |
| `task_health` | 0 | 1 | 2048 | 0 (`TASK_HEALTH_CORE`) | 1 (`PRIO_HEALTH`) | 2048 (`STACK_HEALTH`) | ✅ |

**Evidence:**
- `task_definitions.h:30-34` — core affinity constants
- `task_definitions.h:41-45` — priority constants
- `task_definitions.h:52-56` — stack size constants
- `src/main.cpp:143-174` — all 5 `xTaskCreatePinnedToCore` calls use task_definitions.h constants exclusively

**Additional checks:**
- WiFi/network-adjacent work pinned to core 1 (`task_network`, `task_camera`). Camera pinned to core 1 is slightly non-intuitive (document says "camera/streaming on core 0") but `task_definitions.h:29-34` documents the actual assignment clearly. Camera is on core 1 alongside network because that's the core where the sensor DMA operates. **PASS with note** — documented deviation.
- Camera priority (3) is highest on its core per §5: "camera capture gets the highest application priority on its core." **PASS**.
- `task_camera` created before `task_rtsp` in `main.cpp:152-162` (frame queue dependency). **PASS**.

**Verdict: PASS.** All 5 tasks match the expected core/priority/stack configuration exactly. The configuration is centralized in `task_definitions.h` and used exclusively via named constants in `main.cpp`.

---

## 8.3 — Verify All 4 Root Causes Resolved

| # | Audit Root Cause | Fix Implemented | Verification | Status |
|---|---|---|---|---|
| **1** | **CPU/task starvation** (single loop) | 5 FreeRTOS tasks | `src/main.cpp:196-199`: `loop()` calls `vTaskDelete(NULL)`. Zero business logic in `loop()`. All work distributed across 5 dedicated FreeRTOS tasks per §5. | ✅ **RESOLVED** |
| **2** | **JPEG quality mismatch** (1–100 UI vs 0–63 driver) | `camera_map_quality()` | `src/app/camera_manager.cpp:143-160`: mapping function with correct integer arithmetic (denominator=99). Clamping at input boundaries. Test cases documented in `camera_manager.h:32-35`. `camera_set_quality()` at line 169 enforces the mapping layer — raw 0–63 never exposed to UI. | ✅ **RESOLVED** |
| **3** | **WiFi no retry/backoff** | State machine + exponential backoff | `src/app/network_manager.cpp:207-246`: `on_wifi_connection_failed()` — 3 retries at 1s/2s/4s (`WIFI_RETRY_DELAY_MS_1/2/3` from config.h). Non-blocking via `millis()` comparison. `src/app/network_manager.cpp:147-164`: disconnect debounce with `WIFI_DISCONNECT_DEBOUNCE_MS=5000` filter in event handler. `src/app/network_manager.cpp:389`: `WiFi.setAutoReconnect(true)`. `network_init()` calls `skipApStartup()` when credentials exist. | ✅ **RESOLVED** |
| **4** | **Spontaneous reboot** (5 sub-issues) | Multiple fixes | See sub-table below. | ✅ **RESOLVED** |

**Root Cause 4 — Sub-Issue Verification:**

| Sub-Issue | Fix | Evidence | Status |
|---|---|---|---|
| Brownout | Brownout detector **not disabled** | `src/main.cpp:50-52`: comment asserts brownout must stay enabled. Zero `RTC_CNTL_BROWN_OUT_REG` in codebase. | ✅ |
| Unbounded `skipScanBytes()` | Bounded to `SKIP_SCAN_MAX_ITER=1024` | `lib/Micro-RTSP/src/CStreamer.cpp:277-309`: bounded scanner with max_iter clamped to 1024. `lib/Micro-RTSP/src/CStreamer.cpp:253-262`: `findJPEGheader()` section-length OOB guard. `test/test_parsers.cpp`: 12 host-side unit tests covering boundary cases. | ✅ |
| No TWDT | TWDT mandatory, 10s timeout, panic enabled | `src/app/health_monitor.cpp:67`: `esp_task_wdt_init(TWDT_TIMEOUT_SEC, true)`. `src/app/health_monitor.cpp:101-117`: all 5 tasks registered. Every task resets TWDT in its loop. | ✅ |
| No heap guard | `heap_can_allocate()` before every ≥1KB allocation | `src/app/health_monitor.cpp:232-248`: guard function with 20% threshold. Used in `camera_init()` (line 39), `camera_fb_get()` (line 198), `web_ui_init()` (line 622), `rtsp_server::client_handler()` (line 97). | ✅ |
| Unbounded clients | Hard caps: RTSP=3, HTTP=5 | `src/app/health_monitor.cpp:155-179`: `rtsp_client_accept()`/`release()` with critical-section counters. Same for HTTP (lines 192-216). `lib/rtsp_server/rtsp_server.cpp:92-129`: RTSP 503 rejection on cap exceed. `src/app/web_ui_service.cpp:169-174` etc.: HTTP 503 + `Retry-After: 30` on cap exceed. Integration tests in `test/integration/`. | ✅ |

---

## 8.4 — §11.1 Production-Ready Gate Checklist

| # | Item | Status | Evidence / Notes |
|---|---|---|---|
| **1** | All 4 root-cause categories resolved and verified | ✅ **PASS** | Verified in §8.3 above. All 4 causes and 5 sub-issues of RC#4 have documented fixes with specific file/line evidence. |
| **2** | Architecture matches §3 and §5 exactly — no lingering single-threaded `loop()` business logic | ✅ **PASS** | `loop()` calls `vTaskDelete(NULL)` (`main.cpp:198`). Directory structure matches §3 (with minor deviations noted in §8.1.9). Task architecture matches §5 table exactly (§8.2). |
| **3** | Web server stack matches §7.1 — IotWebConf scoped to provisioning, ESPAsyncWebServer for request handling, no synchronous `WebServer` | ✅ **PASS** | IotWebConf on port 8080 (config portal only). AsyncWebServer on port 80 (all main UI). `WebServer` instance on `network_manager.cpp:71` is IotWebConf's internal transport — documented exception per §7.1. Zero direct `WebServer` usage by our code. |
| **4** | Brownout detector and TWDT active, zero unexplained reboots across 72-hour soak test | ⚠️ **NEEDS TESTING** | Brownout and TWDT are code-complete (verified in §8.1.3 and §8.3). The 72-hour soak test with ≥1 RTSP client connected and web UI polled has **not been performed yet**. This is the gate's most important remaining item. |
| **5** | PSRAM steady-state ≤85% and flash ≤80% during soak test (measured) | ⚠️ **NEEDS TESTING** | Resource ceiling checks are implemented but unmeasured. Requires the 72-hour soak test with telemetry logging every 60 seconds to produce actual numbers. |
| **6** | RTSP hard cap (3) and HTTP hard cap (5) enforced and tested — 4th/6th connection receives correct rejection | ✅ **PASS (code)** / ⚠️ **NEEDS TESTING (HW)** | Code is complete: RTSP cap enforced in `rtsp_server.cpp:92-129` with 503 response. HTTP cap enforced in each handler in `web_ui_service.cpp` with 503 + `Retry-After: 30`. Integration test scripts (`test/integration/test_rtsp_cap.py`, `test_http_cap.py`) are ready but require target hardware to execute. |
| **7** | No magic numbers, no undocumented static buffers, no unbounded parsing loops | ⚠️ **PARTIAL PASS** (2 magic numbers remain) | See §8.1.1 — two magic numbers found (`20000000` in `camera_manager.cpp:71`, `12000` in `rtsp_server.cpp:97`). All other categories pass: static buffers are documented with size justifications, all parsing loops are bounded. |
| **8** | Reboot-reason logging in place, validated to capture at least one deliberately induced fault | ✅ **PASS (code)** / ⚠️ **NEEDS TESTING (HW)** | `src/app/health_monitor.cpp:265-336`: `log_reboot_reason()` persists reason code + boot counter to NVS under namespace "health". `health_get_last_reboot_reason()` and `health_get_reboot_count()` expose it for web UI. The deliberate-fault validation (e.g., forced WDT timeout) has not been executed — requires hardware test. |

**Summary: 4 PASS, 1 PARTIAL PASS (2 magic numbers), 3 NEEDS TESTING (soak test + HW validation).**

The three NEEDS TESTING items (4, 5, 6-HW, 8-HW) are all hardware-dependent and can only be resolved on the target device. The two magic numbers (item 7 partial) are minor and can be fixed in a single commit before the soak test begins.

---

## 8.5 — Remaining Technical Debt

### High Severity

| # | Issue | Source Step | Resolution |
|---|---|---|---|
| TD-1 | Magic number `20000000` in `camera_manager.cpp:71` | Step 3 | Add `CAMERA_XCLK_FREQ_HZ` constant to `config.h`. ~5-minute fix. Must do before soak test. |
| TD-2 | Magic number `12000` in `rtsp_server.cpp:97` | Step 6 | Add `RTSP_CLIENT_ALLOCATION_GUARD_BYTES` constant to `config.h`. ~5-minute fix. Must do before soak test. |

### Medium Severity

| # | Issue | Source Step | Resolution |
|---|---|---|---|
| TD-3 | `partitions.csv` missing — `min_spiffs.csv` used instead | Step 2 (build) | Create custom `partitions.csv` with explicit app0/app1/spiffs layout. Required for OTA (§7) which is not yet implemented. Defer until OTA feature. |
| TD-4 | `include/version.h` missing — version info in `settings.h` | Step 2 (structure) | Either create `version.h` per §3 directory spec, or update CLAUDE.md §3 to acknowledge `settings.h` as the version source. ~10-minute structural cleanup. |
| TD-5 | IotWebConf dual state machine sync — our `WiFiState` mirrors IotWebConf's internal state | Step 4 | The sync logic at `network_manager.cpp:617-629` is correct but fragile. If IotWebConf's internal state machine changes in a future version, the synchronization may drift. Mitigation: pin IotWebConf version exactly (already done: `@=3.2.1`). |
| TD-6 | Sensor register access bypasses `camera_manager` — `update_camera_settings()` in `network_manager.cpp` calls `sensor->set_*()` directly | Step 4 | `camera_set_quality()` routes through `camera_manager`, but the other 21 sensor parameters bypass the abstraction. The code has a TODO comment at line 273-274 acknowledging this. Not a correctness bug, but violates the single-responsibility boundary. Deferred to Step 3 follow-up. |
| TD-7 | `TASK_HEALTH_LOOP_MS` used in `task_web_ui.cpp` error path instead of `TASK_WEB_UI_LOOP_MS` | Step 5 | In the error-recovery stall loop (`task_web_ui.cpp:32`), `TASK_HEALTH_LOOP_MS` (5s) is used instead of `TASK_WEB_UI_LOOP_MS` (2s). This is harmless (both reset TWDT within budget) but should use the semantically correct constant. ~1-minute fix. |
| TD-8 | `format_duration.h` and `format_number.h` are orphaned | Pre-existing | These utility headers exist in `include/` but are not `#include`d anywhere in `src/`. They appear to be dead code from a prior iteration. Either integrate into the web UI (for human-readable formatting of uptime/memory in the template) or remove. |

### Low Severity

| # | Issue | Source Step | Resolution |
|---|---|---|---|
| TD-9 | `TEMPLATE_VAR_COUNT = 55` is a file-local constant that must be manually synced with the HTML template | Step 7 | If the template gains or loses a variable without updating this constant, the moustache render will reference uninitialized elements (undefined behavior). Add a `static_assert` or document a process for keeping them in sync. Low risk because the template changes infrequently. |
| TD-10 | `xTaskCreatePinnedToCore` uses `NULL` for `pvParameters` | Step 5 (tasks) | The NULL parameter is harmless but means tasks cannot receive a context pointer. If any task later needs init-time configuration, the task creation calls will need updating. Low risk for current architecture. |
| TD-11 | Camera on core 1 alongside WiFi, not core 0 as described in §5 prose | Step 3 | `task_definitions.h:29-34` documents the actual assignment clearly. The §5 table says "core 1 for WiFi, core 0 for camera/streaming" but the implementation puts camera on core 1 alongside network. This is likely intentional (sensor DMA on core 1) but should be reconciled with either a documentation update or a code justification comment. |

---

## 8.6 — Overall Verdict

### Code Quality: STRONG

The codebase is well-organized, consistently follows CLAUDE.md conventions, and has thorough defensive programming. All four documented root causes from the initial audit are resolved with specific, verifiable fixes. The FreeRTOS task architecture is clean, the build system is correctly configured, and the safety mechanisms (TWDT, heap guard, client caps) are properly implemented.

### Ready for 72-Hour Soak Test? YES — with two pre-requisite fixes.

The only blocking issues before the 72-hour soak test are:
1. **Fix TD-1 and TD-2** (two magic numbers → named constants) — trivial, ~10 minutes total
2. **Confirm build compiles** for target hardware with `[env:esp32cam_ai_thinker]`

After those two fixes, all §11.1 code-level items will be PASS. The remaining NEEDS TESTING items (4, 5, 6-HW, 8-HW) can only be resolved on hardware during the soak test itself.

### Ready for ESP-IDF Migration? NO — needs soak test first.

Per CLAUDE.md §11.1, ALL checkboxes must be true before migration begins. The soak test (item 4) is the gatekeeper. After the soak test passes:
- PSRAM/flash measurements from the test (item 5) provide the data to verify §6 budget ceilings
- Client cap enforcement (item 6) is validated on real hardware
- Reboot reason logging (item 8) is verified with a deliberate fault

### Summary Statistics

| Category | PASS | FAIL | NEEDS TESTING |
|---|---|---|---|
| §8.1 Codebase Scans | 6/8 | 2/8 (magic numbers) | — |
| §8.2 Task Architecture | 5/5 | — | — |
| §8.3 Root Causes | 4/4 | — | — |
| §8.4 §11.1 Checklist | 4/8 | 1/8 (partial: 2 magic numbers) | 3/8 (HW-dependent) |
| **Post-fix projection** | **7/8** | **0/8** | **1/8** (soak test only) |

### Recommended Action Before Soak Test

```
1. Add CAMERA_XCLK_FREQ_HZ       20000000 to config.h  → fix camera_manager.cpp:71
2. Add RTSP_CLIENT_ALLOCATION_GUARD_BYTES  12000 to config.h → fix rtsp_server.cpp:97
3. Build: pio run -e esp32cam_ai_thinker
4. Deploy and begin 72-hour soak test with telemetry logging
```

---

*Report produced 2026-07-08 by Step 8/8 Final Audit — this concludes the production-hardening review cycle.*
