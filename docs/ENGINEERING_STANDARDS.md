# Engineering Standards — ESP32-CAM RTSP CCTV

Production-grade firmware standard for the **AI-Thinker ESP32-CAM (OV2640, PSRAM 4MB,
flash 4MB)**. PlatformIO + Arduino now; ESP-IDF migration triggers only when the
production-ready gate (§11) is fully satisfied.

Every rule was derived from a systematic audit ([AUDIT_REPORT_V1.md](../AUDIT_REPORT_V1.md),
2026-07-06) that identified four root-cause categories of production defects: CPU/task
starvation, JPEG quality mismatch, WiFi retry/backoff absence, and spontaneous reboot.

---

## 1. Hardware Baseline (Non-Negotiable)

| Parameter | Value | Constraint |
|---|---|---|
| MCU | ESP32 (classic, dual-core Xtensa LX6) | Not S3/S2 — no USB-OTG, no extra SIMD |
| PSRAM | 4 MB, external | Cache-bug workaround required in build flags |
| Flash | 4 MB | OTA needs dual-app partition scheme (§7) |
| Camera | OV2640 | JPEG quality register: 0–63, **lower = higher quality** |
| Power | Mains (prototype) | Brownout detector must remain enabled (§8) |
| Voltage regulator | AMS1117 3.3 V (onboard) | Undersized for WiFi TX spikes — hardware fix, not firmware workaround |

- **PSRAM cache workaround mandatory.** Every build must pass
  `-mfix-esp32-psram-cache-issue` — the ESP32 classic has a hardware PSRAM cache bug.
  *Prevents silent data corruption when WiFi and camera contend for PSRAM.*

- **JPEG quality: never expose raw 0–63 to users.** The OV2640 register uses
  lower = higher quality. A user selecting "80" on a 0–100 slider gets near-zero
  quality because the driver reads it literally. Always insert a mapping layer
  (e.g., `map(ui_value, 1, 100, 63, 0)`). *Prevents the quality-range inversion in
  [AUDIT_REPORT_V1.md §2](../AUDIT_REPORT_V1.md).*

- **Brownout detector stays enabled.** The AMS1117 regulator is undersized for WiFi
  TX spikes (~300 mA). Disabling the detector replaces a clean reset with register
  corruption and non-deterministic crashes. Fix the hardware — add a bulk capacitor
  on the 3.3 V rail. *Prevents the reboot cascade in [AUDIT_REPORT_V1.md §4, Kandidat A](../AUDIT_REPORT_V1.md).*

---

## 2. Build System (PlatformIO)

- **Framework:** Arduino now, ESP-IDF only after the production-ready gate (§11).
  Finish one framework first — partial migration creates two bug surfaces. *Prevents
  split-brain build configurations.*

- **Optimization:** `-Os` only (size-optimized, timing-deterministic). Never `-Ofast`
  or `-O3` — aggressive optimization hides stack/heap bugs and produces indeterminate
  RTSP frame timing. *Prevents timing regressions
  ([AUDIT_REPORT_V1.md §1, item 5](../AUDIT_REPORT_V1.md)).*

- **Log level:** `ARDUHAL_LOG_LEVEL_WARN` for staging, `ARDUHAL_LOG_LEVEL_ERROR` for
  field deployment. `VERBOSE`/`INFO` only in local dev builds, never committed as
  default. UART at 115200 bps is a slow path — verbose logging blocks the calling task.
  *Prevents CPU starvation from serial output under load.*

- **Dependency versions:** Pinned exactly (`lib@=x.y.z`) in production builds. Caret
  ranges (`^x.y.z`) acceptable only during prototype. *Prevents silent breakage from
  upstream changes.*

- **Web server stack fixed:** IotWebConf (provisioning/config only) +
  ESPAsyncWebServer + AsyncTCP (request handling). Do not add the synchronous
  `WebServer` library. *Prevents web/RTSP blocking-I/O contention
  ([AUDIT_REPORT_V1.md §1](../AUDIT_REPORT_V1.md)).*

- **Two environments required** post-prototype: `env:dev` (verbose logging, asserts on)
  and `env:release` (warnings only, asserts off, `-Os`). *Prevents dev-only settings
  from reaching field devices.*

---

## 3. Directory Structure

```
/
├── platformio.ini
├── partitions.csv
├── include/
│   ├── config.h          # compile-time constants (pins, buffers, timeouts)
│   ├── settings.h        # runtime-configurable defaults (IotWebConf-backed)
│   └── version.h         # firmware version string, build metadata
├── src/
│   ├── main.cpp          # setup()/loop() — orchestration only, ≤150 lines
│   ├── app/              # one manager per responsibility, ≤400 lines each
│   └── tasks/            # task_definitions.h + one file per FreeRTOS entry
├── lib/                  # vendored/forked third-party libraries only
└── test/                 # native unit tests for pure logic
```

- `main.cpp` capped at ~150 lines — orchestration only. *Prevents the god-object
  pattern behind the original loop-starvation bug.*

- No `.cpp` exceeding ~400 lines without being split. *Prevents monolithic modules
  that resist unit testing.*

- All FreeRTOS task priorities and stack sizes in `task_definitions.h` — never
  scattered across `xTaskCreate` call sites. *Prevents priority-inversion bugs from
  invisible priority assignments.*

---

## 4. C++ Code Conventions (Embedded C++, ESP32 Idiomatic)

- **Naming:** `snake_case` for functions/variables, `PascalCase` for classes/structs,
  `UPPER_SNAKE_CASE` for compile-time constants and macros. The `k` prefix for
  constexpr (`kMaxRtspClients`) is optional; pick one convention and apply it
  project-wide. *Prevents naming collisions with ESP-IDF macros and improves readability
  at the HAL boundary.*

- **No dynamic allocation after `setup()`.** Exceptions: pooled/pre-sized buffers (§6),
  PSRAM-backed camera frame buffers via `camera_manager`, and heap-guarded allocations
  with a justifying comment. *Prevents heap fragmentation
  ([AUDIT_REPORT_V1.md §4, Kandidat F](../AUDIT_REPORT_V1.md)).*

- **No blocking calls in steady-state task loops** without an explicit
  `vTaskDelay`/`yield`. A task that never yields starves all lower-priority tasks on its
  core. *Prevents the single-client-blocks-all-others problem from the original
  `handle_stream()` loop ([AUDIT_REPORT_V1.md §1, item 1](../AUDIT_REPORT_V1.md)).*

- **No unbounded loops parsing external data.** Every parsing loop over network
  payloads, JPEG streams, or sensor data must have an explicit max-iteration or
  max-length bound and a defined failure return path — created in response to the
  unbounded `while(true)` pointer chase in `skipScanBytes()`
  ([AUDIT_REPORT_V1.md §4, Kandidat B](../AUDIT_REPORT_V1.md)).
  *Prevents Guru Meditation errors from out-of-bounds access on malformed input.*

- **Error handling:** Functions that can fail return `esp_err_t` or a project-defined
  `Result` enum — never silently return default/zero on failure. Reserving exceptions
  for host-side unit tests only. *Prevents silent failure propagation through the call
  stack.*

- **Comments explain why, not what.** A comment restating the code is noise; a comment
  explaining a hardware quirk, range mismatch, or workaround is required. *Prevents
  tribal-knowledge loss when the original author is unavailable.*

- **No magic numbers in `.cpp` files.** Pins, buffer sizes, timeouts, and thresholds
  must be named constants in `config.h` or `task_definitions.h`. *Prevents
  copy-paste drift of critical constants across the codebase.*

- **No raw `new`/`delete` for objects tied to network clients.** Use a fixed-size pool
  sized to the hard client cap (§8). *Prevents unbounded client allocation and heap
  exhaustion ([AUDIT_REPORT_V1.md §4, Kandidat F](../AUDIT_REPORT_V1.md)).*

---

## 5. FreeRTOS Task Architecture

A single cooperative `loop()` is prohibited beyond prototype stage. Minimum task
layout:

| Task | Core | Priority | Stack | Responsibility |
|---|---|---|---|---|
| `task_network` | 1 | 2 | 4096 | WiFi state machine, retry/backoff, MQTT (future) |
| `task_rtsp` | 0 | 2 | 4096–6144 | RTSP session handling, frame dispatch |
| `task_web_ui` | 0 | 1 | 4096 | HTTP handlers, cached template rendering |
| `task_camera` | 1 | 3 | 4096 | Frame capture cadence, buffer handoff |
| `task_health` | 0 or 1 | 1 (idle-adjacent) | 2048 | TWDT reset, heap watch, reboot-reason log |

- **Camera capture gets highest application priority on its core.** Frame cadence is
  the core function — a stalled UI must never stall capture. *Prevents priority
  inversion between streaming and auxiliary services.*

- **Document mutex ownership at the point of declaration.** State which task owns the
  frame buffer mutex and lock acquisition order. *Prevents deadlocks from undocumented
  lock ordering.*

- **No indefinite blocking on sockets.** All socket operations use bounded timeouts;
  every task must yield to TWDT within its configured window. *Prevents a hung TCP
  connection from freezing an entire core.*

- **Pin WiFi/network to core 1, camera/streaming to core 0** unless profiling shows a
  rebalance is needed — document any deviation. *Prevents WiFi/camera PSRAM cache
  contention on the same core.*

---

## 6. Memory & Resource Budget

- **PSRAM ceiling: 85%** of available PSRAM at steady-state runtime. Camera frame
  buffers are the primary consumer — size and count must be recomputed whenever
  resolution or quality defaults change. *Prevents PSRAM exhaustion from silent
  accumulation of frame buffers.*

- **Flash partition ceiling: 80%** of the defined partition. *Prevents OTA update
  failure from insufficient scratch space.*

- **Heap guard before any allocation ≥1 KB:** reject the operation with status 503 if
  `ESP.getFreeHeap()` is below **20% of total heap**. Log the rejection with the reason.
  *Prevents heap-exhaustion crashes ([AUDIT_REPORT_V1.md §4, Kandidat F](../AUDIT_REPORT_V1.md)).*

- **Every static/BSS buffer over 1 KB must carry a justification comment** stating why
  it is static-sized rather than pooled or streamed. Its contribution to the total DRAM
  budget must be tracked in `config.h` as a named constant with a running-total comment.
  *Prevents silent DRAM exhaustion from accumulated static buffers
  ([AUDIT_REPORT_V1.md §4, Kandidat C](../AUDIT_REPORT_V1.md)).*

- **Client connection hard caps:**
  - RTSP: max **3** concurrent clients
  - HTTP/web UI: max **5** concurrent clients
  - Both must return an explicit rejection (503 + `Retry-After` for HTTP; RTSP
    equivalent error response) — never silently drop the connection.
  *Prevents the unbounded-`accept()` denial-of-service vector
  ([AUDIT_REPORT_V1.md §4, Kandidat F](../AUDIT_REPORT_V1.md)).*

---

## 7. Networking, Security & OTA

### 7.1 Web Server Stack (Fixed)

- **IotWebConf retained**, scoped strictly to captive-portal WiFi provisioning, the
  AP↔STA state machine, and NVS-backed config storage — not the request-handling
  layer. *Preserves provisioning without inheriting its blocking HTTP server.*

- **Synchronous `WebServer` replaced with `ESPAsyncWebServer` + `AsyncTCP`.** The
  root cause of web/RTSP contention was blocking request handling sharing the execution
  context with streaming — not IotWebConf's provisioning logic. Async handlers
  eliminate the blocking loop. *Prevents streaming-vs-web starvation
  ([AUDIT_REPORT_V1.md §1](../AUDIT_REPORT_V1.md)).*

- **Template rendering output cached**, re-rendered only when a backing parameter
  changes. *Prevents per-request heap allocation.*

### 7.2 WiFi Connection Behavior

- **Retry with exponential backoff:** minimum 3 attempts (1s/2s/4s) before AP-mode
  fallback. A single `WiFi.begin()` failure must never trigger AP mode alone.
  *Prevents transient RF from forcing unnecessary downtime
  ([AUDIT_REPORT_V1.md §3](../AUDIT_REPORT_V1.md)).*

- **Disconnect debounce:** 5 seconds of continuous `WL_DISCONNECTED` before reconnect
  logic fires. *Prevents reconnect storms from transient RF glitches.*

- **`WiFi.setAutoReconnect(true)`** set at the ESP-IDF WiFi layer. *Prevents staying
  disconnected after transient AP dropout.*

- **Credentials never hardcoded.** NVS-backed storage only — no SSID/password literals
  in any source file. *Prevents credential leaks in version control.*

### 7.3 OTA Gate

- **If/when OTA is added:** signed-payload verification mandatory before flashing;
  dual-app layout (ota_0/ota_1) with rollback support in `partitions.csv`. Do not
  implement OTA without both. *Prevents bricked devices from unsigned firmware.*

---

## 8. Reliability & Recovery

- **Brownout detector: always enabled.** Fix brownouts with hardware (bulk capacitor
  on 3.3 V rail), never by calling `WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0)`.
  *Prevents register corruption from undervoltage
  ([AUDIT_REPORT_V1.md §4, Kandidat A](../AUDIT_REPORT_V1.md)).*

- **Task Watchdog Timer (TWDT): mandatory.** 10-second timeout, panic-on-timeout
  enabled. Every long-running task must register and reset TWDT periodically. A task
  that can't reset within budget is a bug, not a reason to disable the watchdog.
  *Prevents permanent system hang ([AUDIT_REPORT_V1.md §4, Kandidat D](../AUDIT_REPORT_V1.md)).*

- **Reboot reason logged and persisted** (NVS) on every boot: `esp_reset_reason()`
  plus crash counter. *Prevents undiagnosable field failures.*

- **All external-data parsing bounds-checked.** No `while(true)` or pointer-chase
  loops without an explicit max length derived from the buffer's known size.
  *Prevents out-of-bounds access from malformed input
  ([AUDIT_REPORT_V1.md §4, Kandidat B](../AUDIT_REPORT_V1.md)).*

---

## 9. Quality Bar (Measurable, Not Aspirational)

**Robust** — Every malformed-input path has a defined, tested failure mode (no
undefined behavior). Every peripheral fault has explicit detection + recovery or
safe-degradation (no silent hang).

**Reliable** — Brownout detector + TWDT active in every build (§8). No unbounded
resource growth over uptime: heap, static buffers, client count all capped and
monitored (§6). Operating assumptions stated in `config.h` as documented constants
(indoor mains power, ambient temp range typical for enclosure).

**Maintainable** — Calibration constants in `config.h`/`settings.h`, never inline.
Naming convention (§4) applied consistently. Comments explain rationale, not code.
Directory structure (§3) followed for all new modules.

**Scalable** — Deferred. Single-unit deployment only; fleet management and multi-device
provisioning are out of scope until explicitly needed.

---

## 10. Testability

Every module must be demonstrably testable in at least one dimension.

### 10.1 Pure Logic Unit Tests (Host)

Any function computing on scalar/buffer inputs and returning scalar/buffer outputs —
mapping functions, parsers, state transitions, arithmetic — must have unit tests in
`test/`. Pure logic functions must not depend on ESP-IDF/Arduino HAL; refactor to
inject HAL dependencies or separate pure logic from I/O. Framework: PlatformIO
`native` environment + doctest or Unity (`pio test -e native`). *Prevents untestable
logic buried inside I/O functions.*

### 10.2 Module Contract Tests

Every function returning `esp_err_t` or a `Result` enum must have at least one test
verifying its error path. Heap guard, client cap rejection, and TWDT registration
failure each need a trigger-and-verify test case. Mock HAL boundaries only, never
business logic. *Prevents silent failure swallowing.*

### 10.3 Integration Tests (Target Hardware)

Before the 72-hour soak test, run targeted scenarios with scripts in
`test/integration/`: RTSP 4th client → 503; HTTP 6th client → 503 + `Retry-After`;
WiFi disconnect → debounce 5s → reconnect within 15s; camera init failure →
`task_health` logs error, system does not panic. *Prevents regressions in
production-critical failure paths.*

### 10.4 Soak Test (Target Hardware, 72 Hours)

Continuous run with ≥1 RTSP client connected and web UI polled periodically. Metrics
logged every 60 seconds: free heap, PSRAM used, client count, stack high watermarks,
reboot counter. Pass criteria: zero unexplained reboots, PSRAM ≤85%, heap degradation
<5% over 72 hours, all client caps enforced throughout. *Prevents slow resource leaks
that only manifest after days of uptime.*

### 10.5 Testability Code Requirements

- Hardware-interacting functions must expose a test seam: `#ifdef TEST` mock hooks or
  dependency injection (pass handle as parameter, not global). *Prevents untestable
  HAL-coupled code.*
- No function longer than 40 lines unless it is a pure initializer with no branching.
  *Prevents functions too complex to unit-test exhaustively.*
- All assertions in test files must carry a descriptive message string. *Prevents
  unreadable test failure output.*

---

## 11. Production-Ready Gate & ESP-IDF Migration

**Direction:** ESP-IDF migration only after every exit criterion below is met on
PlatformIO. Migration is a reward for reaching the bar, not a parallel track.

### 11.1 Exit Criteria (All Must Be True)

- [ ] All 4 root-cause categories from the initial audit resolved and verified
      (loop/task starvation, JPEG quality mapping, WiFi retry/backoff, spontaneous
      reboot) — see [AUDIT_REPORT_V1.md](../AUDIT_REPORT_V1.md) for the full list.
- [ ] Architecture matches §3 (directory structure) and §5 (FreeRTOS task table)
      exactly — no lingering single-threaded `loop()` business logic.
- [ ] Web server stack matches §7.1 (IotWebConf scoped to provisioning only,
      ESPAsyncWebServer for all request handling) — no synchronous `WebServer` usage.
- [ ] Brownout detector and TWDT active in every build. Zero unexplained reboots
      across a **72-hour continuous soak test** under realistic load.
- [ ] PSRAM steady-state ≤85% and flash partition usage ≤80% during the soak test,
      measured, not assumed.
- [ ] RTSP hard cap (3) and HTTP hard cap (5) enforced and tested — the 4th/6th
      connection attempt verified to receive the correct rejection response.
- [ ] No magic numbers, no undocumented static buffers, no unbounded parsing loops
      remaining in the codebase (§4, §6, §8 compliance pass).
- [ ] Reboot-reason logging validated to correctly capture at least one deliberately
      induced fault.

### 11.2 What Migration Changes

Migration re-implements the framework layer only; settled decisions carry over:
- §3 directory structure → ESP-IDF components, same responsibility boundaries.
- §5 task table → re-validated against ESP-IDF default task overhead, not assumed identical.
- §6 memory ceilings → the 85%/80% policy stays; underlying numbers re-profiled since
  Arduino overhead disappears but ESP-IDF has its own baseline.
- §8 reliability → TWDT API changes (`esp_task_wdt_*` native); mandatory, per-task,
  10s policy unchanged.
- §7 networking → WiFi retry/backoff and IotWebConf-equivalent provisioning must survive.

**Order:** framework port with feature parity only. No new features, no re-architecture.
Optimization passes begin only after parity is confirmed on ESP-IDF.

---

## 12. Hard Constraints (The "Do Not" List)

No exceptions. Each rule was created from a confirmed audit defect or non-negotiable
hardware constraint.

1. **Do not disable the brownout detector**, for any reason. Fix the power supply.
2. **Do not use `-Ofast` or `-O3`** — `-Os` only in any committed build config.
3. **Do not leave `CORE_DEBUG_LEVEL` at `VERBOSE`/`INFO`** in staging or field builds.
4. **Do not pass UI-facing JPEG quality to `esp_camera_init`** without a tested
   mapping function (driver range 0–63, lower = higher quality).
5. **Do not write parsing loops over network/sensor data without an explicit length bound.**
6. **Do not allocate per-client objects with unbounded `new`** — use a capped pool.
7. **Do not hardcode WiFi credentials** — NVS-backed config only.
8. **Do not add MQTT, OTA, or multi-device support speculatively** — implement only
   when needed, after updating this standard.
9. **Do not let `main.cpp` accumulate business logic** — orchestration only (≤150 lines).

---

*Standard version: 2026-07-08. Derived from [AUDIT_REPORT_V1.md](../AUDIT_REPORT_V1.md)
(2026-07-06). All thresholds and constants are pinned; changes require a documented
re-audit of the affected section.*
