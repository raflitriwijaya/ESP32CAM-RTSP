# ESP32-CAM RTSP CCTV — Production-Hardened

A production-grade RTSP surveillance firmware for AI-Thinker ESP32-CAM (OV2640),
hardened from [rzeldent/esp32cam-rtsp](https://github.com/rzeldent/esp32cam-rtsp).

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![FreeRTOS](https://img.shields.io/badge/FreeRTOS-5--task-green)](#architecture)
[![PlatformIO](https://img.shields.io/badge/PlatformIO-6.x-orange)](https://platformio.org/)

---

## About This Fork

The original [esp32cam-rtsp](https://github.com/rzeldent/esp32cam-rtsp) by **Rene Zeldent** is an excellent piece of work: it gets an RTSP stream running on an ESP32-CAM in minutes with a single `.ino` file. It has broad board support, a well-designed web configuration interface, and minimal dependencies. As a prototype and learning resource, it delivers exactly what it promises.

This fork applies systematic embedded-systems engineering to bring that prototype to a **field-deployable production firmware** — the kind you can leave running 24/7 in an enclosure without unexplained reboots, stream lockups, or silent connection drops.

**What this fork adds:**

| Layer | What Changed |
|---|---|
| **Architecture** | Single cooperative `loop()` → 5 dedicated FreeRTOS tasks with pinned cores, explicit priorities, and non-blocking I/O |
| **WiFi Resilience** | Single-attempt → exponential-backoff retry (1s/2s/4s) + 5-second disconnect debounce to prevent reconnect storms |
| **Reliability Triad** | No watchdog + brownout disabled → TWDT (10s panic timeout), brownout detector always enabled, heap guard at 20% free |
| **Memory Safety** | Unlimited allocation → fixed client caps (RTSP 3, HTTP 5) with explicit 503 rejection; PSRAM usage ceiling at 85% |
| **Parser Bounds** | Unbounded `while(true)` loops → every parsing loop has an explicit max-iteration bound and defined failure path |
| **JPEG Quality** | Raw pass-through (UI 1–100 mapped directly to driver 0–63, inverted) → OV2640 datasheet-compliant mapping with documented ranges |
| **Test Coverage** | Zero tests → host-side unit tests for pure logic + Python integration scripts for client-cap enforcement |

**Who this fork is for:**

- Deploying an ESP32-CAM as part of a real surveillance or monitoring setup
- Running 24/7 without periodic watchdog resets or stream dropouts
- Integrating the camera into a larger system where predictable failure
  modes matter more than feature count
- Learning what "production-ready" means on a resource-constrained MCU

If you're just experimenting with ESP32-CAM streaming for the first time, the
[original repo](https://github.com/rzeldent/esp32cam-rtsp) is a better starting
point — it's simpler, lighter, and has broader board support.

---

## What's Different?

| Aspect | Original | This Fork |
|---|---|---|
| **Architecture** | Single `loop()` — all workloads serialized | 5 FreeRTOS tasks with pinned cores and explicit priorities |
| **Web Server** | Synchronous `WebServer` (blocking) | `ESPAsyncWebServer` + `AsyncTCP` (non-blocking) |
| **JPEG Quality** | Raw pass-through (1–100 → 0–63, inverted semantics) | UI-to-driver mapping layer with documented range inversion |
| **WiFi Retry** | Single attempt, immediate AP fallback | 3-attempt exponential backoff (1s/2s/4s) + 5s debounce |
| **Watchdog** | None | TWDT with 10s panic timeout, all tasks registered |
| **Brownout Detector** | Disabled in software (`WRITE_PERI_REG`) | Always enabled — brownout is a hardware fix, not a firmware workaround |
| **Heap Guard** | None | Mandatory guard before any allocation ≥1KB; reject at 20% free |
| **Client Limits** | Unlimited | RTSP: 3 max, HTTP: 5 max — both reject with explicit status on overflow |
| **Template Rendering** | Re-rendered every HTTP request | Cached; re-renders only when backing parameters change |
| **Parser Safety** | Unbounded `while(true)` loops over untrusted data | All parsing loops bounded with `SKIP_SCAN_MAX_ITER` |
| **Debug Log Level** | `VERBOSE` (committed default) | `ERROR` for release, `INFO` for dev — never `VERBOSE` as default |
| **Optimization** | `-Ofast` | `-Os` (timing determinism over raw speed) |
| **PSRAM Cache Fix** | Absent | `-mfix-esp32-psram-cache-issue` in all build targets |
| **Dependency Pinning** | Unpinned (caret ranges) | Exact version pinning (`@=x.y.z`) for production reproducibility |
| **Test Coverage** | None | Host-side unit tests (`pio test -e native`) + Python integration scripts |

---

## Quick Start

### Prerequisites

- [PlatformIO](https://platformio.org/) (IDE extension or CLI)
- AI-Thinker ESP32-CAM (or compatible board)
- USB-to-Serial adapter (e.g., ESP32-CAM-MB, FTDI)

### Build & Upload

```sh
git clone https://github.com/rzeldent/esp32cam-rtsp.git   # swap with this fork's URL
cd esp32cam-rtsp

# Production build (field deployment):
pio run -e release --target upload

# Development build (verbose logging, debug-friendly optimization):
pio run -e dev --target upload

# Monitor serial output:
pio device monitor
```

### WiFi Provisioning

On first boot, the device starts in AP mode:

1. Connect to the WiFi network **ESP32CAM-RTSP**
2. Open [http://192.168.4.1](http://192.168.4.1) (or let the captive portal auto-open)
3. Configure your WiFi credentials and board type
4. Reboot — the device connects to your access point

### Stream URLs

| Protocol | URL |
|---|---|
| **RTSP** | `rtsp://<device-ip>:554/stream` |
| **HTTP MJPEG** | `http://<device-ip>/stream` |
| **HTTP Snapshot** | `http://<device-ip>/snapshot` |
| **Status JSON** | `http://<device-ip>/status` |

For detailed hardware setup (wiring, flash mode, board selection), see the
[original repo's README](https://github.com/rzeldent/esp32cam-rtsp#readme) —
the hardware-level instructions are unchanged.

---

## Documentation

| Document | Description |
|---|---|
| [**Engineering Standards**](CLAUDE.md) | The complete engineering specification this codebase follows — hardware baseline, build system rules, task architecture, memory budget, quality bar definitions, and the ESP-IDF migration gate (§11) |
| [**Audit Report**](AUDIT_REPORT_V1.md) | Root-cause analysis of 4 critical symptom categories found in the original prototype, with evidence tables and fix recommendations |
| [**Integration Tests**](test/integration/README.md) | Python scripts that verify RTSP/HTTP client-cap enforcement on target hardware |
| [**Unit Tests**](test/test_parsers.cpp) | Host-side unit tests for pure-logic functions (parser bounds, JPEG quality mapping) — run with `pio test -e native` |

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│  Core 1 (WiFi / Network)      Core 0 (Streaming / UI)   │
│                                                         │
│  task_network  (prio 2)       task_rtsp      (prio 2)   │
│  ┌───────────────────┐       ┌───────────────────┐     │
│  │ WiFi state machine │       │ RTSP server +     │     │
│  │ IotWebConf doLoop  │       │ frame dispatch to │     │
│  │ MQTT (future)      │       │ up to 3 clients   │     │
│  └───────────────────┘       └─────────┬─────────┘     │
│                                         │                │
│  task_camera   (prio 3)    ◄──Queue──  │                │
│  ┌───────────────────┐                  │                │
│  │ Frame capture     │       task_web_ui (prio 1)       │
│  │ sole fb_get/fb_   │       ┌───────────────────┐     │
│  │ return owner       │       │ AsyncWebServer    │     │
│  └───────────────────┘       │ handlers + cache   │     │
│                               └───────────────────┘     │
│                                                         │
│  task_health   (prio 1, either core)                    │
│  ┌───────────────────┐                                 │
│  │ TWDT reset        │                                  │
│  │ heap watch        │                                  │
│  │ telemetry logging │                                  │
│  └───────────────────┘                                 │
└─────────────────────────────────────────────────────────┘
```

All task priorities, stack sizes, core pinning, and loop intervals are declared
in a single source of truth: [`include/task_definitions.h`](include/task_definitions.h).
No magic numbers at `xTaskCreate` call sites.

---

## How This Was Built

This fork was produced as a **human-AI collaboration** with the human embedded
engineer directing every step:

1. **Starting point:** [rzeldent/esp32cam-rtsp](https://github.com/rzeldent/esp32cam-rtsp) —
   a working RTSP prototype with broad board support.

2. **Systematic audit** identified 4 root-cause categories blocking production
   deployment: CPU/task starvation, JPEG quality mapping inversion, WiFi
   single-attempt fragility, and spontaneous reboot vulnerabilities.
   → [Full audit report](AUDIT_REPORT_V1.md)

3. **Engineering standard** defined in [`CLAUDE.md`](CLAUDE.md) — every change
   since the audit has been bound by these rules. The standard is the contract:
   if a change doesn't follow it, the change is wrong.

4. **Hardening implemented step-by-step:**
   - Step 1: Architecture refactor — single `loop()` → 5 FreeRTOS tasks
   - Step 2: WiFi resilience — exponential backoff + disconnect debounce
   - Step 3: Camera manager — frame buffer lifecycle, JPEG quality mapping
   - Step 4: Network manager — WiFi state machine, IotWebConf integration
   - Step 5: Web UI service — async handlers, template caching
   - Step 6: Health monitor — TWDT, heap guard, reboot-reason logging
   - Step 7: RTSP service — bounded parsers, client caps
   - Step 8: Integration tests + telemetry hooks

5. **AI assistance:** Code generation, audit automation, and review were
   assisted by **Claude Code** (Anthropic) via the **DeepSeek API**. All
   AI-generated output was reviewed, tested, and validated by a human
   embedded-systems engineer before being committed.

6. **The role of AI here is transparent by design** — all AI-assisted
   contributions are traceable to specific `CLAUDE.md` rules and audit
   findings. This is not a "vibe-coded" project; it's a systematically
   engineered one where AI accelerated the mechanical work while the
   human made every engineering decision.

---

## Project Status

- [x] Architecture refactor — 5 FreeRTOS tasks, no `loop()` business logic
- [x] WiFi resilience — exponential backoff + 5s disconnect debounce
- [x] JPEG quality mapping — OV2640 datasheet-compliant, UI range documented
- [x] Reliability triad — brownout (always on), TWDT (10s panic), heap guard (20%)
- [x] Client connection caps — RTSP 3 / HTTP 5 with explicit 503 rejection
- [x] Bounded parsers — `SKIP_SCAN_MAX_ITER` on all untrusted-data loops
- [x] Template caching — re-render only on parameter change
- [x] Reboot-reason logging — persisted to NVS
- [x] Integration test scripts — RTSP & HTTP cap enforcement
- [x] Host-side unit tests — pure-logic parser tests via `pio test -e native`
- [x] Build cleanup — `-Os`, `-mfix-esp32-psram-cache-issue`, exact version pinning
- [ ] 72-hour soak test — per [CLAUDE.md §11.1](CLAUDE.md#111-exit-criteria-must-all-be-true-before-starting-esp-idf-migration)
- [ ] ESP-IDF migration — blocked on all §11.1 exit criteria passing

---

## Credits

- **Original Author:** Rene Zeldent ([@rzeldent](https://github.com/rzeldent)) —
  original ESP32-CAM RTSP firmware, web configuration interface, and multi-board
  support. This project exists because his prototype was well-designed enough to
  be worth hardening.

- **Production Hardening:** Rafli Triwijaya ([Rafli Triwijaya](https://github.com/raflitriwijaya)) —
  systematic audit, architecture redesign, hardening implementation, and
  integration test suite.

- **AI Assistance:** Claude Code (Anthropic) via DeepSeek API —
  code generation, audit automation, and review. All AI output was
  human-reviewed and validated.

---

## License

[GPLv3](LICENSE) — inherited from the original
[rzeldent/esp32cam-rtsp](https://github.com/rzeldent/esp32cam-rtsp) project.
