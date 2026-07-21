// config.h — Single source of truth for ALL hardware pins and runtime constants.
//
// CLAUDE.md §3: compile-time constants (pins, buffer sizes, timeouts) live here.
// No magic numbers anywhere else in the codebase after this file exists.
//
// CLAUDE.md §1: -mfix-esp32-psram-cache-issue is required in ALL build targets
// at all times. This flag is set in platformio.ini, not here — if you add a new
// build target, ensure it carries that flag.
//
// Naming: UPPER_SNAKE_CASE per CLAUDE.md §4.

#ifndef CONFIG_H
#define CONFIG_H

// ---------------------------------------------------------------------------
// Boot / Setup Timing
// ---------------------------------------------------------------------------
// USB_SETTLE_DELAY_MS: delay after Serial.begin() when ARDUINO_USB_CDC_ON_BOOT
// is defined. Allows the host USB stack to enumerate and attach before we start
// printing log output. Acceptable as setup-only per CLAUDE.md §4 exception.
// ---------------------------------------------------------------------------

#define USB_SETTLE_DELAY_MS              5000  // USB CDC enumeration settle time (5s)

// ---------------------------------------------------------------------------
// Camera Pins — AI-Thinker ESP32-CAM (CAMERA_MODEL_AI_THINKER)
// ---------------------------------------------------------------------------
// Pinout matches the official ESP32 Arduino camera_pins.h for
// CAMERA_MODEL_AI_THINKER. Source:
//   framework-arduinoespressif32/libraries/ESP32/examples/Camera/
//   CameraWebServer/camera_pins.h
// ---------------------------------------------------------------------------

#define CAM_PIN_Y2         5   // D0  — Y2_GPIO_NUM
#define CAM_PIN_Y3        18   // D1  — Y3_GPIO_NUM
#define CAM_PIN_Y4        19   // D2  — Y4_GPIO_NUM
#define CAM_PIN_Y5        21   // D3  — Y5_GPIO_NUM
#define CAM_PIN_Y6        36   // D4  — Y6_GPIO_NUM
#define CAM_PIN_Y7        39   // D5  — Y7_GPIO_NUM
#define CAM_PIN_Y8        34   // D6  — Y8_GPIO_NUM
#define CAM_PIN_Y9        35   // D7  — Y9_GPIO_NUM
#define CAM_PIN_XCLK       0   // Master clock to camera (XCLK_GPIO_NUM)
#define CAM_PIN_PCLK      22   // Pixel clock from camera (PCLK_GPIO_NUM)
#define CAM_PIN_HREF      23   // Horizontal reference (HREF_GPIO_NUM)
#define CAM_PIN_VSYNC     25   // Vertical sync (VSYNC_GPIO_NUM)
#define CAM_PIN_SIOD      26   // SCCB data / SDA (SIOD_GPIO_NUM)
#define CAM_PIN_SIOC      27   // SCCB clock / SCL (SIOC_GPIO_NUM)
#define CAM_PIN_PWDN      32   // Power-down (PWDN_GPIO_NUM)
#define CAM_PIN_RESET     -1   // Reset (RESET_GPIO_NUM — not connected on AI-Thinker)
#define CAM_PIN_LED_FLASH  4   // White LED flash (LED_GPIO_NUM — GPIO 4)

// ---------------------------------------------------------------------------
// JPEG Quality Ranges (OV2640 driver)
// ---------------------------------------------------------------------------
// IMPORTANT: The OV2640 JPEG quality register uses range 0–63 where LOWER = HIGHER
// quality (0 = best, 63 = worst). Never expose raw 0–63 to end users without
// mapping. The UI uses 1–100 where HIGHER = BETTER quality.
// CLAUDE.md §1, §12.
// ---------------------------------------------------------------------------

#define JPEG_QUALITY_UI_MIN         1    // UI slider minimum (lowest quality)
#define JPEG_QUALITY_UI_MAX         100  // UI slider maximum (highest quality)
#define JPEG_QUALITY_DRIVER_MIN     0    // Driver value for HIGHEST quality
#define JPEG_QUALITY_DRIVER_MAX     63   // Driver value for LOWEST quality
#define JPEG_QUALITY_DEFAULT        8    // Default driver value (good balance)

// ---------------------------------------------------------------------------
// Camera Init Retry Policy
// ---------------------------------------------------------------------------
// CLAUDE.md §8: every peripheral fault must have explicit detection + recovery.
// If the camera sensor fails to init (I2C glitch, power-on race), retry before
// giving up. After all retries are exhausted, the system continues to boot —
// the camera-dependent tasks will detect the failed init and degrade gracefully.
// ---------------------------------------------------------------------------

#define CAMERA_INIT_RETRY_COUNT         3     // Max retry attempts for camera_init()
#define CAMERA_INIT_RETRY_DELAY_MS      500   // Delay between retry attempts (500ms)

// ---------------------------------------------------------------------------
// Camera Heap Guard Thresholds
// ---------------------------------------------------------------------------
// CLAUDE.md §6: heap guard mandatory before any allocation ≥1KB at runtime.
// These are the estimated allocation sizes for camera operations.
// - CAMERA_INIT_HEAP_GUARD_BYTES: conservative DRAM estimate for esp_camera_init()
//   internal state + 2 SVGA frame buffers in PSRAM (128KB).
// - CAMERA_FB_HEAP_GUARD_BYTES: conservative estimate for a single SVGA JPEG
//   frame buffer allocation (typically 30–80KB, guard at 64KB minimum).
// ---------------------------------------------------------------------------

#define CAMERA_INIT_HEAP_GUARD_BYTES    131072  // 128KB — camera init (internal + 2 framebuffers)
#define CAMERA_FB_HEAP_GUARD_BYTES      65536   // 64KB — single SVGA JPEG frame buffer

// ---------------------------------------------------------------------------
// Camera SCCB / I2C Configuration
// ---------------------------------------------------------------------------
// The AI-Thinker ESP32-CAM uses I2C_NUM_0 for the OV2640 SCCB interface.
// ESP32 has two I2C controllers (0 and 1); we specify 0 explicitly to avoid
// ambiguity on boards where the driver default might differ.
// ---------------------------------------------------------------------------

#define CAMERA_SCCB_I2C_PORT            0      // I2C_NUM_0 — OV2640 SCCB interface

// ---------------------------------------------------------------------------
// Client Connection Hard Caps
// ---------------------------------------------------------------------------
// CLAUDE.md §6: reject excess connections with explicit status, never silently drop.
// ---------------------------------------------------------------------------

#define MAX_RTSP_CLIENTS   3   // Hard cap for concurrent RTSP sessions
#define MAX_HTTP_CLIENTS   5   // Hard cap for concurrent HTTP/web UI sessions

// ---------------------------------------------------------------------------
// Memory Guard
// ---------------------------------------------------------------------------
// CLAUDE.md §6: heap guard mandatory before any allocation ≥1KB at runtime.
// Reject/503 the operation if free heap drops below this fraction of total heap.
// ---------------------------------------------------------------------------

#define HEAP_GUARD_THRESHOLD   0.2f   // 20% of total heap (must be > 0.0, <= 1.0)

// ---------------------------------------------------------------------------
// RTSP Server Timing
// ---------------------------------------------------------------------------
// CLAUDE.md §5: RTSP task runs on core 0, priority 2, with non-blocking tick.
// The RTSP server's internal timer (client_handler) fires every
// RTSP_SERVER_TICK_INTERVAL_MS to service clients and broadcast frames.
// The task loop calls doLoop() (timer_.tick()) at RTSP_LOOP_INTERVAL_MS to
// check for due timers promptly without busy-waiting.
// RTSP_SERVER_TICK_INTERVAL_MS matches DEFAULT_FRAME_DURATION (200ms = 5fps)
// from settings.h — the frame broadcast cadence.
// ---------------------------------------------------------------------------

#define RTSP_LOOP_INTERVAL_MS             40   // Task loop period (drives doLoop())
#define RTSP_SERVER_TICK_INTERVAL_MS      200  // Internal timer period (frame broadcast cadence, 5fps)

// ---------------------------------------------------------------------------
// Camera Frame Queue (Producer-Consumer Architecture)
// ---------------------------------------------------------------------------
// CLAUDE.md TD-7: single-producer (task_camera), single-consumer (task_rtsp)
// via a FreeRTOS queue. task_camera is the sole owner of esp_camera_fb_get()
// and esp_camera_fb_return(). task_rtsp receives pre-captured frame pointers
// and broadcasts them to RTSP clients.
// ---------------------------------------------------------------------------

#define CAMERA_FB_COUNT                    2    // Frame buffer count (double-buffered)
#define CAMERA_FRAME_QUEUE_LENGTH          CAMERA_FB_COUNT  // Queue depth (must match fb_count)
#define CAMERA_CAPTURE_INTERVAL_MS         40   // Target ~25fps capture cadence
#define CAMERA_QUEUE_SEND_TIMEOUT_MS       100  // Max wait when queue is full (backpressure)

// Frame-dimension FALLBACK only. task_rtsp derives the streamer's real
// dimensions at runtime from the configured frame size via
// network_get_frame_size() + the esp32-camera resolution[] table, so any size
// chosen in the web UI (VGA, SVGA, UXGA, ...) works. These constants are used
// only if that lookup returns FRAMESIZE_INVALID. OV2640Streamer::streamPreCaptured()
// also sets the live dimensions from each fb, so the RTP header always matches
// the actual frame regardless of this value.
#define CAMERA_FRAME_WIDTH                 640   // VGA width  (fallback only)
#define CAMERA_FRAME_HEIGHT                480   // VGA height (fallback only)

// Capacity of the shared "latest frame" snapshot buffer (PSRAM). task_camera
// publishes each captured JPEG into it so the web /snapshot and /stream handlers
// can serve frames WITHOUT calling esp_camera_fb_get() — keeping task_camera the
// sole owner of the camera driver and avoiding frame-buffer contention with the
// RTSP path (CLAUDE.md §3 producer/consumer). Sized generously for an SVGA JPEG;
// a frame larger than this simply isn't published (web snapshot returns 503).
#define CAMERA_LATEST_FRAME_BYTES          (128 * 1024)   // 128 KB

// Compile-time check: CAMERA_FRAME_QUEUE_LENGTH must equal CAMERA_FB_COUNT.
// Frame buffers are a scarce resource (PSRAM-backed, ~50KB each at SVGA).
// The queue depth and fb_count must match so that at most fb_count frames are
// in flight at any time — a deeper queue would cause buffer starvation at the
// driver level, and a shallower queue would waste a pre-allocated frame buffer.
static_assert(CAMERA_FRAME_QUEUE_LENGTH == CAMERA_FB_COUNT,
    "CAMERA_FRAME_QUEUE_LENGTH must equal CAMERA_FB_COUNT");

// ---------------------------------------------------------------------------
// Task Watchdog Timer (TWDT)
// ---------------------------------------------------------------------------
// CLAUDE.md §8: TWDT is mandatory with panic-on-timeout. Every long-running task
// must register and periodically reset within this window.
// ---------------------------------------------------------------------------

#define TWDT_TIMEOUT_SEC   10   // TWDT panic timeout in seconds

// Telemetry log interval: how many health task iterations between detailed
// telemetry dumps. task_health loops every TASK_HEALTH_LOOP_MS (5s), so
// every 12th iteration = ~60 seconds. This avoids flooding the serial console
// while still providing regular diagnostics for soak tests (CLAUDE.md §10.2).
#define TELEMETRY_LOG_INTERVAL  12   // Detailed telemetry every N health-loop iterations

// ---------------------------------------------------------------------------
// WiFi Connection Policy
// ---------------------------------------------------------------------------
// CLAUDE.md §7.2: retry with exponential backoff before AP fallback.
// Disconnect must be debounced to avoid reconnect storms.
// ---------------------------------------------------------------------------

#define WIFI_RETRY_COUNT               3     // Max connection attempts before AP mode fallback
#define WIFI_RETRY_DELAY_MS_1          1000  // First retry delay
#define WIFI_RETRY_DELAY_MS_2          2000  // Second retry delay
#define WIFI_RETRY_DELAY_MS_3          4000  // Third retry delay
#define WIFI_DISCONNECT_DEBOUNCE_MS    5000  // Continuous disconnect before triggering reconnect

// ---------------------------------------------------------------------------
// Web Server Port Assignment (CLAUDE.md §7.1)
// ---------------------------------------------------------------------------
// IotWebConf's synchronous WebServer runs on IOTWEBCONF_WEB_PORT (provisioning
// config portal only). The main web UI (AsyncWebServer) runs on port 80.
// In AP/provisioning mode, AsyncWebServer on port 80 redirects captive-portal
// requests to IotWebConf's config portal on the port below.
// ---------------------------------------------------------------------------

#define IOTWEBCONF_WEB_PORT           8080  // IotWebConf config portal (provisioning only)
#define WEB_UI_PORT                   80    // AsyncWebServer main web UI port

// ---------------------------------------------------------------------------
// Web UI Buffer Sizing
// ---------------------------------------------------------------------------
// STATUS_JSON_BUF_SIZE: fixed buffer for the /status JSON payload (~11 fields,
// all short strings/ints). 512 bytes is sufficient with ~40% headroom.
// ASYNC_WEB_SERVER_ESTIMATED_BYTES: estimated sizeof(AsyncWebServer) + internal
// AsyncTCP buffer for the listening socket. Used as the heap guard threshold
// before the `new AsyncWebServer` allocation in web_ui_init().
// ---------------------------------------------------------------------------

#define STATUS_JSON_BUF_SIZE             512   // snprintf buffer for /status JSON
#define ASYNC_WEB_SERVER_ESTIMATED_BYTES (sizeof(AsyncWebServer) + 4096)  // server obj + internal buffers

// ---------------------------------------------------------------------------
// Template Cache Configuration
// ---------------------------------------------------------------------------
// CLAUDE.md §7.1: moustache_render() output must be cached. Re-render only
// when backing parameters change (settings save, WiFi state change).
// MAX_TEMPLATE_SIZE_BYTES provides the upper bound for the cached HTML string.
// ---------------------------------------------------------------------------

#define MAX_TEMPLATE_SIZE_BYTES       8192  // 8KB — current template renders ~2.6KB

// ---------------------------------------------------------------------------
// Bounded Parsing
// ---------------------------------------------------------------------------
// CLAUDE.md §4, §8: every parsing loop over external/untrusted data must have an
// explicit max-iteration bound and a defined failure return path. No while(true)
// or pointer-chasing without an explicit length derived from the buffer's known size.
//
// NOTE: skipScanBytes() (JPEG EOI scan) does NOT use this constant as its limit.
// Its correct bound is the JPEG scan-buffer length itself (passed as *len) —
// "an explicit length derived from the buffer's known size", exactly what §8
// requires. A fixed 1024 cap was WRONG for that loop: a full-resolution frame's
// entropy data runs tens of KB before the EOI marker, so 1024 rejected every
// real frame. This constant is retained only as a defensive cap for other
// small, bounded scans, and by the host-side parser unit tests.
// ---------------------------------------------------------------------------

#define SKIP_SCAN_MAX_ITER   1024  // Defensive cap for small bounded scans (NOT the JPEG EOI scan)

// ---------------------------------------------------------------------------
// Build Flag Reminder
// ---------------------------------------------------------------------------
// per-platformio.ini build_flags and CLAUDE.md §1:
//   -mfix-esp32-psram-cache-issue    (MANDATORY — ESP32 classic PSRAM cache bug)
//   -Os                              (production — timing determinism over raw speed)
//   -Og                              (dev only — debug-friendly optimization)
//   -Ofast / -O3                     (FORBIDDEN per CLAUDE.md §2, §12)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Brownout detector — ALWAYS enabled (CLAUDE.md §8, §12: never disable it).
// The former TEST_DISABLE_BROWNOUT flag has been removed. If brownout resets
// occur, the fix is HARDWARE (bulk capacitor on the 3.3V rail / a supply that
// can handle WiFi TX spikes on the undersized AMS1117), never disabling the
// protection in firmware.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Build Flag Reminder
// ---------------------------------------------------------------------------
// per-platformio.ini build_flags and CLAUDE.md §1:
//   -mfix-esp32-psram-cache-issue    (MANDATORY — ESP32 classic PSRAM cache bug)
//   -Os                              (production — timing determinism over raw speed)
//   -Og                              (dev only — debug-friendly optimization)
//   -Ofast / -O3                     (FORBIDDEN per CLAUDE.md §2, §12)
// ---------------------------------------------------------------------------

#endif // CONFIG_H
