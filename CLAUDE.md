# CLAUDE.md — ESP32-CAM RTSP CCTV Project Standard

Persistent engineering standard for this project. Applies to all current and future
work on this codebase. Target: production-ready firmware on **AI-Thinker ESP32-CAM
(OV2640, PSRAM 4MB, flash 4MB)**, PlatformIO + Arduino framework now, migrating to
ESP-IDF at production-ready stage.

This document encodes constraints derived from the initial audit (2026-07-06).
Any deviation from a rule below must be justified in a code comment referencing
the rule number.

---

## 1. Hardware Baseline (Non-Negotiable Facts)

| Parameter | Value | Note |
|---|---|---|
| MCU | ESP32 (classic, dual-core Xtensa LX6) | Not S3/S2 — no USB-OTG, no extra SIMD |
| PSRAM | 4MB, external, cache-bug-affected | `-mfix-esp32-psram-cache-issue` required in build flags at all times |
| Flash | 4MB | OTA needs dual-app partition scheme (see §7) |
| Camera | OV2640 | JPEG quality register range is **0–63, lower = higher quality** — never expose raw 0–63 to end users without a mapping layer |
| Power | Mains (prototype) | Brownout detector must remain enabled regardless — see §6 |
| Voltage regulator | AMS1117 3.3V (onboard, undersized for WiFi TX spikes) | Hardware-level issue; firmware must not paper over it by disabling protections |

---

## 2. Build System (PlatformIO)

- Framework: **Arduino** during prototype/pre-production. ESP-IDF migration happens
  only at the production-ready gate (§9) — do not start a partial migration mid-project.
- Mandatory `platformio.ini` settings:
  - `build_flags`: `-Os` (never `-Ofast` or `-O3` — timing determinism over raw speed)
  - `CORE_DEBUG_LEVEL`: `ARDUHAL_LOG_LEVEL_WARN` for staging, `ARDUHAL_LOG_LEVEL_ERROR`
    for anything built for field deployment. `VERBOSE`/`INFO` only for local dev builds,
    never committed as default.
  - Board definition must retain `-mfix-esp32-psram-cache-issue`.
- Every dependency version must be pinned exactly (`lib@=x.y.z`), no caret ranges in
  production builds. Caret ranges (`^x.y.z`) are acceptable only in prototype stage.
- Web server stack is fixed to **IotWebConf (provisioning/config only) + ESPAsyncWebServer +
  AsyncTCP (request handling)** — see §7.1 for the full rationale and integration rule.
  Do not add the synchronous `WebServer` core library as a dependency.
- Two PlatformIO environments required once past prototype stage: `env:dev` (verbose
  logging, asserts on) and `env:release` (warnings only, asserts off, `-Os`).

---

## 3. Directory Structure

```
/
├── platformio.ini
├── partitions.csv                 # explicit partition table (see §7)
├── include/
│   ├── config.h                   # compile-time constants (pins, buffer sizes, timeouts)
│   ├── settings.h                 # runtime-configurable defaults (IotWebConf-backed)
│   └── version.h                  # firmware version string, build metadata
├── src/
│   ├── main.cpp                   # setup()/loop() — orchestration ONLY, no business logic
│   ├── app/
│   │   ├── camera_manager.{h,cpp}     # camera init, frame buffer lifecycle, quality mapping
│   │   ├── network_manager.{h,cpp}    # WiFi lifecycle, retry/backoff state machine
│   │   ├── rtsp_service.{h,cpp}       # RTSP session orchestration (wraps Micro-RTSP)
│   │   ├── web_ui_service.{h,cpp}     # HTTP handlers, template rendering + caching
│   │   ├── mqtt_service.{h,cpp}       # (future) device monitoring/telemetry
│   │   └── health_monitor.{h,cpp}     # heap guard, TWDT registration, reboot reason logging
│   └── tasks/
│       ├── task_definitions.h     # all task handles, priorities, stack sizes in ONE place
│       └── task_*.cpp             # one file per FreeRTOS task entry function
├── lib/                            # vendored/forked third-party libs only (Micro-RTSP etc.)
└── test/                           # native unit tests for pure logic (mapping functions, parsers)
```

Rules:
- `main.cpp` must not contain business logic. It creates tasks, initializes managers,
  and returns. If `main.cpp` exceeds ~150 lines, logic has leaked into it — refactor.
- No god-files: any `.cpp` exceeding ~400 lines must be split by responsibility.
- All FreeRTOS task priorities and stack sizes live in **one file**
  (`task_definitions.h`), never scattered inline at each `xTaskCreate` call site.

---

## 4. C++ Code Conventions (Embedded C++, ESP32 idiomatic)

- **Naming**: `snake_case` for functions/variables, `PascalCase` for classes/structs,
  `UPPER_SNAKE_CASE` for compile-time constants and macros, `k` prefix optional for
  constexpr (`kMaxRtspClients`) — pick one convention and apply it project-wide.
- **No dynamic allocation after `setup()` completes**, except:
  - Explicitly pooled/pre-sized buffers (see §6)
  - PSRAM-backed camera frame buffers managed exclusively by `camera_manager`
  - Any exception must be heap-guarded (§6) and documented with a comment stating why
    pre-allocation was not feasible.
- **No blocking calls inside any task's steady-state loop** without an explicit
  `vTaskDelay`/`yield` — a task that never yields starves the scheduler on its core.
- **No unbounded loops parsing external/untrusted data** (network payloads, JPEG
  streams). Every parsing loop must have an explicit max-iteration or max-length bound
  and a defined failure return path — this is a hard rule after the `skipScanBytes()`
  finding in the audit.
- **Error handling convention**: functions that can fail return `esp_err_t` or a
  project-defined `Result` enum — never silently return default/zero values on failure.
  Reserve exceptions for host-side unit tests only (not on-device).
- **Comments explain "why", not "what"**. A comment restating the code is noise;
  a comment explaining a hardware quirk, a range mismatch, or a workaround is required.
- **Magic numbers are forbidden** in `.cpp` files — pin numbers, buffer sizes, timeouts,
  and thresholds must be named constants in `config.h` or `task_definitions.h`.
- **No raw `new`/`delete` for objects with unbounded lifetime tied to network clients**
  (e.g., per-RTSP-client objects) — use a fixed-size pool sized to the hard client cap (§8).

---

## 5. FreeRTOS Task Architecture

Single cooperative `loop()` is prohibited beyond prototype stage. Minimum task layout:

| Task | Core | Priority | Stack (bytes) | Responsibility |
|---|---|---|---|---|
| `task_network` | 1 | 2 | 4096 | WiFi state machine, retry/backoff, MQTT (future) |
| `task_rtsp` | 0 | 2 | 4096–6144 | RTSP session handling, frame dispatch to clients |
| `task_web_ui` | 0 | 1 | 4096 | HTTP handlers, cached template rendering |
| `task_camera` | 1 | 3 | 4096 | Frame capture cadence, hands buffer to consumers |
| `task_health` | 0 or 1 | 1 (idle-adjacent) | 2048 | TWDT reset, heap watch, reboot-reason logging |

Rules:
- **Every task's priority and stack size must be declared explicitly** in
  `task_definitions.h` — no implicit Arduino `loop()` task reliance for anything
  beyond thin orchestration.
- **Camera capture gets the highest application priority** on its core — frame
  cadence is the product's core function; a stalled UI must never stall capture.
- **Document mutex/critical-section ownership** at the point of declaration: which
  task owns the frame buffer mutex, in what order locks are acquired if more than one
  is needed (to prevent priority inversion / deadlock).
- **No task may block indefinitely on a socket call** — all socket operations use a
  bounded timeout, and the task must yield to TWDT within its configured timeout window.
- Pin WiFi/network-adjacent work to core 1, camera/streaming-adjacent work to core 0,
  unless profiling shows a specific rebalance is needed — document any deviation.

---

## 6. Memory & Resource Budget

- **PSRAM usage ceiling: 85%** of available PSRAM at any steady-state runtime moment.
  Camera frame buffers are the primary consumer — size and count must be recomputed
  whenever resolution/quality defaults change.
- **Flash partition usage ceiling: 80%** of the defined partition (see `partitions.csv`, §7).
- **Heap guard is mandatory** before any allocation ≥1KB at runtime (RTSP client
  objects, rendered HTML buffers, JSON payloads): reject/503 the operation if
  `ESP.getFreeHeap()` is below **20% of total heap**. Log the rejection with reason.
- **Static/BSS buffers must be justified individually** — each buffer over 1KB
  (e.g., RTSP request/response buffers) needs a comment stating why it's static-sized
  rather than pooled or streamed, and its contribution to total DRAM budget must be
  tracked in `config.h` as a named constant with a comment showing the running total.
- **Client connection hard caps** (derived from audit finding on unbounded `accept()`):
  - RTSP: max 3 concurrent clients
  - HTTP/web UI: max 5 concurrent clients
  - Both rejections return an explicit status (503 + `Retry-After` for HTTP; RTSP
    equivalent error response) — never silently drop the connection.

---

## 7. Networking, Security & OTA

### 7.1 Web Server Stack Decision (Fixed)

- **IotWebConf is retained**, scoped strictly to: captive-portal WiFi provisioning,
  AP↔STA state machine, and NVS-backed config parameter storage. It is **not** the
  request-handling layer.
- **The synchronous `WebServer` (ESP32 Arduino core) that IotWebConf drives by default
  is replaced with `ESPAsyncWebServer` + `AsyncTCP`.** Rationale: the audit's root
  cause for web/RTSP contention was blocking, synchronous request handling sharing
  the same execution context as streaming — not IotWebConf's config/provisioning logic
  itself. Async handling removes the blocking-loop problem without discarding
  IotWebConf's provisioning value.
- Integration rule: IotWebConf must be initialized in **non-blocking mode**
  (`iotWebConf.doLoop()` called from `task_network`, never from a task that also
  serves streaming), and all HTTP route handlers (root page, parameter form, stream
  endpoints if any remain HTTP-based) are registered as `AsyncWebServerHandler`
  callbacks, not synchronous handlers.
- `moustache_render()` (or equivalent templating) output must be cached and only
  re-rendered when a backing parameter changes — this applies regardless of sync/async
  server, per the original audit finding.
- **Do not** reintroduce the synchronous `WebServer` object anywhere in the codebase
  once this migration lands — `ESPAsyncWebServer` is the only HTTP server class in use.
- `task_web_ui` (§5) becomes primarily a supervisory task under this model — async
  callbacks run in the AsyncTCP task context, not `task_web_ui` itself; `task_web_ui`
  is responsible for IotWebConf's non-streaming `doLoop()` housekeeping and any
  request-independent UI state updates.

### 7.2 WiFi Connection Behavior

- **WiFi connection state machine must implement retry with exponential backoff**:
  minimum 3 attempts, delays 1s/2s/4s, before falling back to AP/config mode.
  A single failed `WiFi.begin()` must never be the sole trigger for AP fallback.
- **Disconnect detection must be debounced** — require WiFi status to report
  disconnected continuously for at least 5 seconds before triggering reconnect logic,
  to avoid reconnect storms from transient RF glitches.
- **`WiFi.setAutoReconnect(true)` must be set** at the ESP-IDF WiFi layer.
- **Credentials are never hardcoded** in source. Use IotWebConf-managed config storage
  (NVS-backed) exclusively. No SSID/password literals in `.cpp`/`.h` files, including
  test/dev builds.
- **RTSP and (future) MQTT payload/schema must be explicit** — every message type
  documented (field names, types, units) in `include/` as a struct or schema comment,
  not left implicit in serialization code.
- **QoS/retry/backoff for MQTT (when implemented)**: minimum QoS 1 for state-changing
  telemetry, explicit reconnect backoff mirroring the WiFi policy above.
- **If/when OTA is introduced**: signed-payload verification is mandatory before flashing,
  and the partition table (`partitions.csv`) must define a dual-app (ota_0/ota_1) layout
  with rollback support — do not add OTA without this in place.
- **User-visible connection status required**: web UI must expose current WiFi state,
  last error/reason code, and retry count — serial-monitor-only diagnostics are not
  acceptable for a field-deployed device. This status page is served via the async
  handler stack defined in §7.1.

---

## 8. Reliability & Recovery

- **Brownout detector must remain enabled** in every build target, including prototype.
  If brownouts occur, the fix is hardware (bulk capacitor on the 3.3V rail, better
  supply), never `WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0)` or equivalent.
- **Task Watchdog Timer (TWDT) is mandatory**: initialize with a 10-second timeout,
  panic-on-timeout enabled. Every long-running task must register with TWDT and reset
  it periodically inside its loop — a task that cannot reset TWDT within its budget
  is a bug to fix, not a reason to disable TWDT.
- **Reboot reason must be logged and persisted** (e.g., to NVS) on every boot —
  `esp_reset_reason()` result plus a lightweight crash counter, so field failures are
  diagnosable without a live serial session.
- **All external/untrusted-data parsing (JPEG stream, RTP, HTTP input, RTSP requests)
  must be bounds-checked** — no `while(true)` or pointer-chasing loops without an
  explicit maximum length derived from the buffer's known size.

---

## 9. Quality Bar Definitions (Measurable, Not Adjectives)

**Robust**
- No undefined behavior on malformed/out-of-range input (sensor glitch, malformed
  JPEG, malformed network payload) — every such path has a defined, tested failure mode.
- Every peripheral/sensor fault (camera init failure, WiFi hardware fault) has an
  explicit detection + recovery or safe-degradation path, not a silent hang.

**Reliable**
- Brownout + TWDT active in every build (§8).
- No unbounded resource growth over uptime: heap, static buffer usage, and client
  count are all capped and monitored (§6).
- Operating assumptions (indoor mains power, ambient temp range typical for enclosure)
  stated in `config.h` as documented constants, not left implicit.

**Maintainable**
- Calibration/config constants (JPEG quality mapping, timeouts, buffer sizes) live in
  `config.h`/`settings.h`, never inline in logic files.
- Naming convention from §4 applied consistently across the codebase.
- Comments explain rationale, not restate code.
- Directory structure from §3 followed for all new modules.

**Scalable**
- Not required for current single-unit deployment. Do not add fleet-management,
  multi-device provisioning, or central-config abstractions until a task prompt
  explicitly requests multi-unit deployment support. Revisit this section when that
  becomes relevant.

---
## 10. Testability (Fifth Quality Pillar)

"Robust," "Reliable," and "Maintainable" are unverifiable without testing.
Every module must be demonstrably testable in at least one dimension below.

### 10.1 Pure Logic Unit Tests (Host)

- Any function that performs computation on scalar/buffer inputs and returns
  scalar/buffer outputs — mapping functions, parsers, state transitions,
  arithmetic — MUST have a corresponding unit test in `test/`.
- Pure logic functions MUST NOT depend on ESP-IDF/Arduino HAL. If a function
  calls `delay()`, `digitalWrite()`, `malloc()` directly, refactor it to
  inject those dependencies or separate pure logic from I/O.
- Test framework: PlatformIO `native` environment + doctest or Unity.
  Run with: `pio test -e native`.

### 10.2 Module Contract Tests (Target or Mocked)

- Every function returning `esp_err_t` or a `Result` enum MUST have at least
  one test verifying its error path (failure is not silent).
- Heap guard, client cap rejection, TWDT registration failure — each has a
  test case that triggers the failure condition and verifies the correct
  error code/response.
- If mocking is required, mock only the HAL boundary, not business logic.

### 10.3 Integration Tests (Target Hardware)

- Before the 72-hour soak test, run targeted integration scenarios:
  - RTSP 4th client → rejected with 503
  - HTTP 6th client → rejected with 503 + Retry-After
  - WiFi disconnect → debounce 5s → reconnect within 15s
  - Camera init failure → task_health logs error, system does not panic
- Each scenario has a short Python/shell script in `test/integration/`.

### 10.4 Soak Test (Target Hardware, 72 Hours)

- Per CLAUDE.md §11.1: 72-hour continuous run with ≥1 RTSP client connected
  and web UI polled periodically.
- Metrics logged every 60 seconds to Serial (or NVS): free heap, PSRAM used,
  client count, stack high watermarks, reboot counter.
- Pass criteria: zero unexplained reboots, PSRAM ≤85%, heap degradation <5%
  over 72 hours, all client caps enforced throughout.

### 10.5 Testability Code Requirements

- Functions that interact with hardware MUST expose a test seam:
  either a `#ifdef TEST` block with mock hooks, or a dependency injection
  pattern (pass sensor handle as parameter, not global).
- No function shall be longer than 40 lines unless it is a pure initializer
  with no branching — longer functions are harder to unit-test and MUST be
  split.
- All assertions in test files MUST have a descriptive message string.
---

## 11. Production-Ready Gate & ESP-IDF Migration Trigger

This project has one direction: **PlatformIO + Arduino now → ESP-IDF migration only
after this file's standard is fully met on PlatformIO.** Migration is a reward for
reaching the bar, not a parallel track and not an escape hatch for unresolved issues.

### 11.1 Exit Criteria (must ALL be true before starting ESP-IDF migration)

- [ ] All 4 root-cause categories from the initial audit are resolved and verified
      (loop/task starvation, JPEG quality mapping, WiFi retry/backoff, spontaneous reboot).
- [ ] Architecture matches §3 (directory structure) and §5 (FreeRTOS task table) exactly
      — no lingering single-threaded `loop()` business logic.
- [ ] Web server stack matches §7.1 (IotWebConf scoped to provisioning only,
      ESPAsyncWebServer for all request handling) with no synchronous `WebServer` usage.
- [ ] Brownout detector and TWDT are active in every build target, with zero
      unexplained reboots across a **72-hour continuous soak test** under realistic load
      (≥1 RTSP client connected, web UI polled periodically).
- [ ] PSRAM steady-state usage stays ≤85% and flash partition usage stays ≤80%
      (§6) during the soak test, measured, not assumed.
- [ ] RTSP hard cap (3 clients) and HTTP hard cap (5 clients) enforced and tested —
      the 4th/6th connection attempt is verified to receive the correct rejection response.
- [ ] No magic numbers, no undocumented static buffers, no unbounded parsing loops
      remain anywhere in the codebase (§4, §6, §8 compliance pass).
- [ ] Reboot-reason logging (§8) is in place and has been validated to correctly
      capture at least one deliberately induced fault (e.g., forced watchdog timeout in test).

Do not begin ESP-IDF migration with any box unchecked. If a criterion is blocked,
fix it on PlatformIO/Arduino first — do not carry a known defect across frameworks.

### 11.2 What ESP-IDF Migration Changes (and What It Doesn't)

Migration re-implements the framework layer; it does **not** re-open decisions already
settled in this file. Carried over unchanged:
- Directory structure (§3) — adapted to ESP-IDF's component model, same responsibility
  boundaries (`camera_manager`, `network_manager`, `rtsp_service`, etc. become components).
- FreeRTOS task table (§5) — priorities/stacks are re-validated against ESP-IDF's own
  default task overhead (main task, WiFi/LWIP tasks), not assumed identical.
- Memory budget ceilings (§6) — re-measured under ESP-IDF, since Arduino-layer overhead
  disappears but ESP-IDF has its own baseline consumption; the 85%/80% ceilings stay the
  policy, the underlying numbers get re-profiled.
- Reliability requirements (§8) — TWDT API changes from Arduino wrapper to native
  `esp_task_wdt_*`, but the requirement itself (mandatory, per-task, 10s timeout) is unchanged.
- Networking policy (§7) — WiFi retry/backoff and IotWebConf-equivalent provisioning
  behavior must be preserved; if IotWebConf itself is dropped in favor of a native
  ESP-IDF provisioning component (e.g., `wifi_provisioning`), the same captive-portal +
  NVS-config guarantees must be re-verified, not silently lost in the port.

Migration order: do the framework/build-system port first with feature parity as the
only goal (no new features, no re-architecture during the port itself). Only after
parity is confirmed on ESP-IDF should any further optimization pass begin.

### 11.3 Post-Migration Scope Unlock

Only after ESP-IDF migration is complete and parity-verified does §9's "Scalable"
section become eligible for revision — fleet/multi-unit deployment features (if ever
needed) are ESP-IDF-era work, not PlatformIO/Arduino-era work.

## 12. Hard "Do Not" List

- Do not disable the brownout detector, ever, for any reason.
- Do not use `-Ofast`/`-O3` in any committed build configuration.
- Do not leave `CORE_DEBUG_LEVEL` at `VERBOSE`/`INFO` in a build meant for staging or field use.
- Do not pass UI-facing JPEG quality values directly to `esp_camera_init` without an
  explicit, tested mapping function (0–63 driver range, lower = higher quality).
- Do not write parsing loops over network/sensor data without an explicit bound.
- Do not allocate per-client objects with unbounded `new` — use a capped pool.
- Do not hardcode WiFi credentials anywhere in source.
- Do not add MQTT, OTA, or multi-device support speculatively — implement only when
  a task prompt requests it, and only after this file's relevant section is updated.
- Do not let `main.cpp` accumulate business logic — orchestration only.