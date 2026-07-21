# Architecture

## Overview

ESP32-CAM RTSP CCTV uses a 5-task FreeRTOS architecture with a producer-consumer
frame pipeline. Camera capture and streaming are decoupled via a FreeRTOS queue
(`g_frame_queue`, `src/tasks/task_camera.cpp:30`). WiFi, web UI, and health
monitoring each run in isolated tasks with well-defined priorities and core
affinities declared in `include/task_definitions.h`.

The original single-threaded `loop()` architecture serialized all work: WiFi,
web server, RTSP, and camera capture competed for a single execution context.
An MJPEG HTTP client could block RTSP streaming entirely (audit finding #1 in
`AUDIT_REPORT_V1.md`). Five dedicated tasks with preemptive scheduling guarantee
that streaming continues regardless of web UI activity. The original `loop()` is
explicitly killed at `src/main.cpp:197` (`vTaskDelete(NULL)`).

## Task Model

All values are from `include/task_definitions.h`. Stack sizes are in FreeRTOS
words (×4 = bytes on ESP32 Xtensa). Each task entry function lives in
`src/tasks/` and is a thin orchestration loop — business logic lives in the
corresponding manager module under `src/app/`.

| Task | Core | Priority | Stack (words) | Period | Responsibility |
|---|---|---|---|---|---|
| `task_camera` | 1 | 3 | 4096 | 40ms | Frame capture via `camera_fb_get()`, enqueue to `g_frame_queue` |
| `task_network` | 1 | 2 | 4096 | 1s | WiFi state machine, `IotWebConf.doLoop()` housekeeping |
| `task_rtsp` | 0 | 2 | 6144 | 40ms | RTSP session handling, frame broadcast via `broadcastFrame()` |
| `task_web_ui` | 0 | 1 | 4096 | 2s | Cached status JSON rebuild, template cache invalidation |
| `task_health` | 0 | 1 | 2048 | 5s | TWDT reset, heap monitoring, reboot-reason telemetry |

All five tasks are registered with the Task Watchdog Timer
(`TWDT_REGISTERED_TASK_COUNT`, `include/task_definitions.h:82`).

### Priority Rationale

- **`task_camera` (prio 3):** Highest application priority on core 1. Frame
  cadence is the product's core function — a stalled UI or network must never
  stall capture. Camera gets first claim on CPU time on its core.
- **`task_network` (prio 2):** WiFi state machine on core 1, same core as camera
  but lower priority. `network_tick()` and `IotWebConf.doLoop()` are periodic
  and yield-friendly — they don't need to preempt capture.
- **`task_rtsp` (prio 2):** RTSP session handling on core 0, isolated from the
  camera pipeline. Same priority as network but on a different core — no
  contention. The 6144-word stack (largest in the system) accommodates the
  `rtsp_server` object, its internal `std::list<rtsp_client>`, and the
  `OV2640Streamer`/`CRtspSession` objects created per-client.
- **`task_web_ui` (prio 1):** Lowest application priority. Template cache
  invalidation and status JSON rebuild are low-urgency background work.
  `AsyncTCP` handles the actual HTTP I/O in its own task context, so this
  task never blocks on sockets.
- **`task_health` (prio 1):** Idle-adjacent. Heap checks and telemetry logging
  can always wait a few seconds. Detailed telemetry is logged every 12th
  iteration (~60s; `TELEMETRY_LOG_INTERVAL`, `include/config.h:171`).

### Core Affinity Rationale

- **Core 1:** Camera + Network. Camera capture is CPU-bound (JPEG encoding at
  SVGA). WiFi state machine logic is lightweight — placing it on core 1 avoids
  cross-core cache line bouncing for PSRAM access, since both tasks interact
  with PSRAM-backed resources (frame buffers). The ESP-IDF LWIP stack runs
  primarily on core 0, so socket-heavy tasks (RTSP, HTTP) are kept off core 1.
- **Core 0:** RTSP + Web UI + Health. Streaming and HTTP handling share core 0
  with the underlying TCP/IP stack, minimizing cross-core synchronization
  overhead for socket operations. The `AsyncTCP` callback context also runs on
  core 0, so HTTP handler execution is local to this core.

## Inter-Task Communication

### Frame Pipeline (Producer-Consumer)

```
task_camera (producer)              task_rtsp (consumer)
      │                                     │
      │ camera_fb_get()                     │
      │      ↓                              │
      │ xQueueSend() ────→ [Queue: 2] ──────→ xQueueReceive()
      │      ↓                                     ↓
      │ camera_fb_get()                    broadcastFrame(fb)
      │ (next frame)                               ↓
      │                                    camera_fb_return(fb)
```

- **Queue handle:** `g_frame_queue` (defined `src/tasks/task_camera.cpp:30`,
  `extern` in `src/tasks/task_rtsp.cpp:43`)
- **Queue length:** 2 (`CAMERA_FRAME_QUEUE_LENGTH`, `include/config.h:146`),
  matching `CAMERA_FB_COUNT`. Enforced at compile time by `static_assert`
  at `include/config.h:155-156`
- **Element type:** `camera_fb_t*` (pointer to PSRAM-backed JPEG frame buffer)
- **Producer:** `task_camera` calls `xQueueSend()` with a 100ms timeout
  (`CAMERA_QUEUE_SEND_TIMEOUT_MS`, `include/config.h:148`). If the queue is
  full and the timeout expires, the frame is dropped and returned to the driver.
  `CAMERA_GRAB_LATEST` ensures the sensor provides a fresh frame on the next
  capture. **Trade-off: freshness over backlog** — a slow consumer causes frame
  drops, not buffer starvation.
- **Consumer:** `task_rtsp` calls `xQueueReceive()` with a zero timeout
  (non-blocking, `src/tasks/task_rtsp.cpp:131`). If no frame is available, it
  skips broadcast and proceeds to RTSP protocol housekeeping. This prevents
  the RTSP control path from stalling when the camera is idle (e.g., before
  any client sends `PLAY`).
- **Buffer ownership:** Only `task_camera` calls `camera_fb_get()`. Only
  `task_rtsp` calls `camera_fb_return()`. The queue transfers ownership of
  the frame pointer. At no point is a buffer returned while still being read
  — `broadcastFrame()` is synchronous, so all clients finish reading before
  `camera_fb_return()` is called.
- **Task creation order:** `task_camera` is created before `task_rtsp` in
  `main.cpp` (`src/main.cpp:149-156`) so `g_frame_queue` exists before the
  consumer tries to read from it. `task_rtsp` verifies this at startup
  (`src/tasks/task_rtsp.cpp:85-89`) and suspends itself if the queue is null.

### WiFi State Notifications (Polling)

```
task_network                          task_web_ui
      │                                     │
      │ network_tick()                      │
      │  → updates internal state           │
      │      ↓                              │
      │ [network_manager state] ←─── network_get_status()
      │                                      ↓
      │                              g_last_wifi_state != status.state?
      │                                      ↓ (yes)
      │                              web_ui_invalidate_cache()
```

- `task_web_ui` polls `network_get_status()` (`include/network_manager.h:63`)
  every 2 seconds (`TASK_WEB_UI_LOOP_MS`)
- On state change, it calls `web_ui_invalidate_cache()` so the next `GET /`
  serves a re-rendered template reflecting the current WiFi status
- An event-group-based push mechanism is planned (`src/tasks/task_network.cpp:38-39`
  TODO) to eliminate the 2-second polling delay; the polling approach was chosen
  first for simplicity and will be replaced when sub-second UI responsiveness
  to WiFi state changes becomes a requirement

## Data Flow

### RTSP Stream (Primary)

```
OV2640 Sensor → PSRAM Frame Buffer → task_camera → Queue → task_rtsp
                                                                │
                                                       broadcastFrame(fb)
                                                                │
                                            OV2640Streamer::streamPreCaptured()
                                                                │
                                            RTP Packetization → UDP/TCP → Client
```

- `task_camera` captures at ~25 fps (40ms interval, `CAMERA_CAPTURE_INTERVAL_MS`,
  `include/config.h:147`)
- `task_rtsp` broadcasts at ~5 fps (200ms tick, `RTSP_SERVER_TICK_INTERVAL_MS`,
  `include/config.h:134`), picking up the latest available frame from the queue
  each time the internal timer fires
- `broadcastFrame()` (`lib/rtsp_server/rtsp_server.h:22`) calls
  `OV2640Streamer::streamPreCaptured()` which reads `fb->buf` / `fb->len`
  directly — **`OV2640::run()` is never called during streaming**. The internal
  frame acquisition path is fully bypassed (`src/tasks/task_rtsp.cpp:14-18`)
- The `OV2640` wrapper instance (static in `src/tasks/task_rtsp.cpp:73`) is
  retained only for dimension discovery when new clients connect — the
  `OV2640Streamer` constructor calls `m_cam.run()` once to read width/height,
  then immediately releases that frame

### HTTP Request (Secondary)

```
Browser → HTTP GET /    → AsyncWebServer (:80) → cached template (static String)
                                                            ↑
                                            invalidated by task_web_ui
                                            on config change or WiFi state change

Browser → HTTP GET /status → AsyncWebServer → web_ui_update_status()
                                                       │
                                        network_get_status() + health_get_*()
```

- `ESPAsyncWebServer` handles all HTTP I/O in `AsyncTCP`'s task context (core 0).
  `task_web_ui` calls `web_ui_update_status()` and `web_ui_invalidate_cache()`
  but never blocks on socket calls
- Template cache (`MAX_TEMPLATE_SIZE_BYTES`, 8KB, `include/config.h:219`) holds
  the rendered HTML. Re-render happens only when a backing parameter changes
  (IotWebConf config saved, WiFi state transition). Current template renders
  ~2.6KB — well within the 8KB ceiling
- The `/status` endpoint returns JSON built from `network_get_status()` and
  `health_get_*()` queries. Buffer size: `STATUS_JSON_BUF_SIZE` = 512 bytes
  (`include/config.h:208`)

## Module Dependency Graph

```
main.cpp
├── camera_manager     (camera init, quality mapping, fb_get/fb_return)
├── network_manager    (WiFi state machine, IotWebConf NVS config, retry/backoff)
├── health_monitor     (TWDT, heap guard, client caps, reboot logging)
├── web_ui_service     (AsyncWebServer, route handlers, template cache, /status JSON)
└── rtsp_server (lib/) (RTSP session handling, RTP packetization, client management)

task_camera  ──→ camera_manager
task_network ──→ network_manager
task_rtsp    ──→ rtsp_server + camera_manager
task_web_ui  ──→ web_ui_service + network_manager + health_monitor
task_health  ──→ health_monitor
```

`main.cpp` (`src/main.cpp:48-188`) creates all five tasks, initializes the camera
(with 3-retry loop), and calls `health_init()`. No business logic beyond
orchestration — it's 140 lines including whitespace and comments.

## Memory Layout

| Region | Consumer | Budget |
|---|---|---|
| DRAM (~320KB usable) | Task stacks, BSS, heap | `HEAP_GUARD_THRESHOLD` = 20% (~64KB) minimum free (`config.h:119`) |
| PSRAM (4MB) | Camera frame buffers (2 × ~300KB SVGA JPEG) | ≤85% steady-state (CLAUDE.md §6) |
| Flash (4MB) | Firmware + SPIFFS (NVS config) | ≤80% partition (CLAUDE.md §6) |

**Task stack budget:** 4096 + 6144 + 4096 + 4096 + 2048 = 20,480 words =
81,920 bytes (~80KB DRAM). Stack high watermarks are logged every ~60 seconds
by `task_health` (`src/tasks/task_health.cpp:70-89`) for field diagnostics.

**Static buffers:** The `rtsp_server` contributes ~26KB of BSS from internal
buffers (documented in `AUDIT_REPORT_V1.md` §4c). The template cache
(`MAX_TEMPLATE_SIZE_BYTES`, 8KB) and status JSON buffer (`STATUS_JSON_BUF_SIZE`,
512 bytes) are heap-allocated, not BSS.

**Heap guard** is enforced at two levels:
1. `heap_can_allocate(size)` (`include/health_monitor.h:73`) — generic guard
   for any runtime allocation ≥1KB
2. `camera_fb_get()` (`include/camera_manager.h:50`) — camera-specific guard
   that returns `nullptr` if free heap < 20% of total

## Client Connection Caps

| Protocol | Max | Constant | Enforcement Point | Rejection Response |
|---|---|---|---|---|
| RTSP | 3 | `MAX_RTSP_CLIENTS` | `rtsp_client_accept()` in `include/health_monitor.h:43` | RTSP error response |
| HTTP | 5 | `MAX_HTTP_CLIENTS` | `http_client_accept()` in `include/health_monitor.h:59` | HTTP 503 + `Retry-After` |

Both caps are defined in `include/config.h:109-110`. Counters are thread-safe;
`rtsp_client_release()` / `http_client_release()` are called when sessions end.

## Key Design Decisions

### Why FreeRTOS tasks instead of a single `loop()`?

A single `loop()` serializes all work — WiFi, web server, RTSP, and camera
capture compete for one execution context. An MJPEG HTTP client blocked RTSP
streaming entirely in the original architecture because the synchronous
`WebServer`'s request handler never yielded. Five dedicated tasks with
preemptive scheduling and well-defined core affinities guarantee that streaming
continues regardless of web UI activity.

### Why producer-consumer for frames?

The original RTSP server called `OV2640::run()` internally from `CRtspSession`'s
streaming loop. Two consumers (or even one consumer with two call sites) would
compete for the 2 frame buffers. A single producer (`task_camera`) with a
FreeRTOS queue eliminates contention and makes frame ownership explicit. The
queue depth equals `CAMERA_FB_COUNT` (2), enforced by `static_assert` at
`include/config.h:155-156`.

### Why non-blocking queue receive in task_rtsp?

`xQueueReceive` in `src/tasks/task_rtsp.cpp:131` uses a timeout of 0
(non-blocking). This ensures the RTSP protocol control path — accepting new
clients, handling `DESCRIBE`/`SETUP`/`PLAY`/`TEARDOWN` requests, cleaning up
disconnected clients — remains responsive even when no frames are being
produced (e.g., before any client sends `PLAY`). The trade-off is that a frame
arriving during a `doLoop()` call sits in the queue for at most one loop
iteration (40ms) before being picked up.

### Why 100ms send timeout (not indefinite) in task_camera?

`xQueueSend` in `src/tasks/task_camera.cpp:81` blocks for at most 100ms. If
the RTSP consumer is too slow to drain the queue, the frame is dropped and
returned to the driver. With only 2 frame buffers, blocking indefinitely would
deadlock the camera driver. `CAMERA_GRAB_LATEST` ensures the next capture gets
a fresh frame, not a stale one. **Trade-off: frame freshness over guaranteed
delivery.**

### Why AsyncWebServer instead of synchronous WebServer?

The synchronous `WebServer` (Arduino core) blocks its calling task during
request handling — a slow TCP client stalls the entire task. Under
`ESPAsyncWebServer` + `AsyncTCP`, all socket I/O runs in `AsyncTCP`'s own
task context. `task_web_ui` only updates cached state and never blocks on
sockets. Per CLAUDE.md §7.1, `IotWebConf`'s internal synchronous `WebServer`
on port 8080 (`IOTWEBCONF_WEB_PORT`, `include/config.h:195`) is tolerated
because it is used only for one-time captive-portal provisioning, not for
steady-state request handling.

## Reliability Architecture

- **TWDT (Task Watchdog Timer):** 10-second timeout (`TWDT_TIMEOUT_SEC`,
  `include/config.h:165`), panic-on-timeout. All 5 tasks are registered
  (`TWDT_REGISTERED_TASK_COUNT`, `include/task_definitions.h:82`) and reset
  TWDT in their steady-state loops. Initialized by `health_init()`
  (`src/main.cpp:182`) after all task handles exist.
- **Reboot reason logging:** `esp_reset_reason()` read at boot
  (`src/main.cpp:76-87`), persisted to NVS with a counter via
  `health_init()` → `log_reboot_reason()` (`include/health_monitor.h:81`).
  Exposed via `health_get_last_reboot_reason()` and `health_get_reboot_count()`.
- **WiFi retry/backoff:** 3 attempts at 1s/2s/4s delays
  (`WIFI_RETRY_DELAY_MS_1/2/3`, `include/config.h:181-183`) before AP mode
  fallback. Disconnect debounced at 5 seconds (`WIFI_DISCONNECT_DEBOUNCE_MS`,
  `include/config.h:184`). `WiFi.setAutoReconnect(true)` set at the ESP-IDF
  layer.
- **Bounded parsing:** `SKIP_SCAN_MAX_ITER` = 1024 (`include/config.h:229`) —
  every parsing loop over external/untrusted data (JPEG streams, RTP, HTTP
  input, RTSP requests) has an explicit iteration bound and a defined failure
  return path. No `while(true)` or unbounded pointer-chasing loops exist
  anywhere in the codebase.
- **Brownout detector:** must remain enabled in every build target
  (CLAUDE.md §8, §12). If brownouts occur, the fix is hardware (bulk
  capacitor on the 3.3V rail), never a firmware workaround.

## Future: ESP-IDF Migration

Per CLAUDE.md §11.2, the task architecture, memory budgets, and reliability
requirements are designed to carry over unchanged to ESP-IDF. The migration
re-implements the framework layer but does **not** re-open architectural
decisions. Specifically preserved:

- 5-task model with identical priority, core affinity, and stack assignments
- Producer-consumer frame pipeline via FreeRTOS queue (`g_frame_queue`)
- `AsyncWebServer` + `AsyncTCP` as the HTTP stack (may migrate to native
  `esp_http_server` if performance testing shows a benefit)
- `HEAP_GUARD_THRESHOLD` (20%) and PSRAM ceiling (85%)
- TWDT with panic-on-timeout, 10-second window
- All client connection caps and bounded-parsing limits
- `IotWebConf` may be replaced with ESP-IDF's native `wifi_provisioning`
  component, preserving the same captive-portal + NVS-config guarantees
