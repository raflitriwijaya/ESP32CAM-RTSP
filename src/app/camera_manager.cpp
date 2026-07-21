// camera_manager.cpp — Camera init, JPEG quality mapping, frame buffer lifecycle.
//
// CLAUDE.md §3: camera_manager owns camera init, frame buffer lifecycle, and
// quality mapping. This is the single source of truth for all camera hardware
// interaction — no other module touches esp_camera_* APIs directly.
//
// CLAUDE.md §1: OV2640 JPEG quality register is 0–63, lower = higher quality.
// The mapping in camera_map_quality() translates the UI-facing 1–100 scale.
//
// CLAUDE.md §6: heap guard is mandatory before any allocation ≥1KB. Frame buffers
// in PSRAM exceed this threshold, so camera_fb_get() enforces the guard.

#include "camera_manager.h"
#include "config.h"
#include "health_monitor.h"
#include "network_manager.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

// ---------------------------------------------------------------------------
// camera_init — Initialize camera with production defaults.
//
// Pinout: AI-Thinker ESP32-CAM standard (XCLK=GPIO0, LED_FLASH=GPIO4).
// The pin assignments use the CAM_PIN_* constants from config.h, which are the
// documented reference for this board. These match the standard AI-Thinker
// ESP32-CAM pinout.
//
// DCW (downscale) is explicitly disabled after init for sharpness, per the
// audit finding that DEFAULT_DCW=true reduces image quality unnecessarily.
// Users can re-enable DCW via the web UI if they need the framerate tradeoff.
// ---------------------------------------------------------------------------
esp_err_t camera_init()
{
    // Heap guard per CLAUDE.md §6: reject if memory is critically low.
    // Camera framebuffers (2 × SVGA JPEG ≈ 120KB) are in PSRAM.
    // The driver also needs some DRAM for its internal state (~16KB).
    // Check PSRAM for framebuffer space (primary consumer) + DRAM for driver.
    if (ESP.getFreePsram() < CAMERA_INIT_HEAP_GUARD_BYTES) {
        log_e("camera_init: PSRAM low — free=%u, need=%u",
              ESP.getFreePsram(), CAMERA_INIT_HEAP_GUARD_BYTES);
        return ESP_ERR_NO_MEM;
    }
    if (!heap_can_allocate(16 * 1024)) {  // 16KB DRAM for camera driver internal state
        log_e("camera_init: DRAM low — heap guard tripped");
        return ESP_ERR_NO_MEM;
    }

    camera_config_t config = {};

    // Pin assignments — AI-Thinker ESP32-CAM standard mapping.
    // CAM_PIN_* values are defined in include/config.h.
    config.pin_pwdn     = CAM_PIN_PWDN;       // -1 (not connected)
    config.pin_reset    = CAM_PIN_RESET;      // -1 (not connected)
    config.pin_xclk     = CAM_PIN_XCLK;       // GPIO 0
    config.pin_sccb_sda = CAM_PIN_SIOD;       // GPIO 18 (SCCB data / SDA)
    config.pin_sccb_scl = CAM_PIN_SIOC;       // GPIO 23 (SCCB clock / SCL)

    config.pin_d7 = CAM_PIN_Y9;   // GPIO 34
    config.pin_d6 = CAM_PIN_Y8;   // GPIO 13
    config.pin_d5 = CAM_PIN_Y7;   // GPIO 14
    config.pin_d4 = CAM_PIN_Y6;   // GPIO 35
    config.pin_d3 = CAM_PIN_Y5;   // GPIO 39
    config.pin_d2 = CAM_PIN_Y4;   // GPIO 38
    config.pin_d1 = CAM_PIN_Y3;   // GPIO 37
    config.pin_d0 = CAM_PIN_Y2;   // GPIO 36

    config.pin_vsync  = CAM_PIN_VSYNC;   // GPIO 5
    config.pin_href   = CAM_PIN_HREF;    // GPIO 27
    config.pin_pclk   = CAM_PIN_PCLK;    // GPIO 25

    // Clock: 10 MHz for maximum stability across ribbon cable quality.
    // 20 MHz is the OV2640 spec max but prone to signal integrity issues.
    config.xclk_freq_hz = 10000000;       // 10 MHz XCLK — conservative, reliable
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;

    // Frame size is fetched dynamically from the web UI settings.
    // Smaller frames = faster DMA, less PSRAM, quicker WiFi TX.
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size   = network_get_frame_size();
    config.jpeg_quality = 12;               // Medium quality, smaller files

    // fb_count=2: double-buffered. With VGA, frame buffers ~30KB each.
    config.fb_count    = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;

    // CAMERA_GRAB_LATEST: if a new frame arrives before we've processed the
    // previous one, drop the old frame. Correct behavior for RTSP streaming —
    // we always want the freshest frame, not a backlog of stale ones.
    config.grab_mode = CAMERA_GRAB_LATEST;

    // I2C/SCCB port. ESP32 has two I2C controllers (0 and 1).
    // The AI-Thinker board uses I2C_NUM_0 for the camera SCCB interface.
    // Using -1 lets the driver pick the default, but we specify 0 explicitly
    // to avoid ambiguity on boards where the default might differ.
    config.sccb_i2c_port = CAMERA_SCCB_I2C_PORT;

    esp_err_t result = esp_camera_init(&config);
    if (result != ESP_OK) {
        log_e("camera_init: esp_camera_init failed with error 0x%x", result);
        return result;
    }

    // Disable DCW (downscale) for sharper images.
    // The OV2640's DCW feature performs hardware downscaling which reduces
    // sharpness. Per the audit (AUDIT_REPORT_V1.md §2), the default was
    // DCW=true, degrading image quality unnecessarily given available PSRAM.
    // Users who need higher framerates at lower resolutions can re-enable
    // DCW through the web UI.
    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        s->set_dcw(s, 0);
    } else {
        log_w("camera_init: unable to get sensor handle — DCW not disabled");
    }

    log_i("camera_init: success — SVGA, JPEG q=%d, fb_count=%d, PSRAM, DCW=off",
          JPEG_QUALITY_DEFAULT, CAMERA_FB_COUNT);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// camera_map_quality — Map UI-facing quality (1–100) to OV2640 driver (63–0).
//
// Mapping formula (integer arithmetic):
//   driver = (DRIVER_MAX * (UI_MAX - clamped)) / (UI_MAX - UI_MIN)
//
// This ensures the endpoints map correctly:
//   UI=100 → driver = (63 * 0) / 99 = 0   (highest quality, largest file)
//   UI=1   → driver = (63 * 99) / 99 = 63  (lowest quality, smallest file)
//   UI=50  → driver = (63 * 50) / 99 = 31  (midpoint → ~midpoint)
//   UI=80  → driver = (63 * 20) / 99 = 12  (high quality)
//
// The denominator is (UI_MAX - UI_MIN) = 99, not 100, because the UI range
// spans 99 discrete steps (1 through 100 inclusive). Using 100 would produce
// UI=1 → driver=62 instead of 63, an off-by-one at the low-quality endpoint.
//
// CLAUDE.md §1, §12: the raw 0–63 range must NEVER be exposed to end users
// without this mapping layer.
// ---------------------------------------------------------------------------
uint8_t camera_map_quality(uint8_t ui_quality)
{
    // Clamp to valid UI range.
    if (ui_quality < JPEG_QUALITY_UI_MIN) {
        ui_quality = JPEG_QUALITY_UI_MIN;
    }
    if (ui_quality > JPEG_QUALITY_UI_MAX) {
        ui_quality = JPEG_QUALITY_UI_MAX;
    }

    // Linear mapping: higher UI value → lower driver value (higher quality).
    // Integer division is intentional — OV2640 quality register is integer 0–63.
    uint8_t driver_quality = (JPEG_QUALITY_DRIVER_MAX
                              * (JPEG_QUALITY_UI_MAX - ui_quality))
                             / (JPEG_QUALITY_UI_MAX - JPEG_QUALITY_UI_MIN);

    return driver_quality;
}

// ---------------------------------------------------------------------------
// camera_set_quality — Set JPEG quality at runtime using UI-scale value.
//
// This allows the web UI's IotWebConf parameter callback to apply quality
// changes without requiring a camera deinit/reinit cycle. The mapping is
// handled internally so the caller always works in the 1–100 UI scale.
// ---------------------------------------------------------------------------
esp_err_t camera_set_quality(uint8_t ui_quality)
{
    sensor_t* sensor = esp_camera_sensor_get();
    if (sensor == nullptr) {
        log_e("camera_set_quality: sensor handle is null — camera not initialized?");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t driver_q = camera_map_quality(ui_quality);
    sensor->set_quality(sensor, driver_q);

    log_i("camera_set_quality: UI=%u → driver=%u", ui_quality, driver_q);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// camera_fb_get — Acquire a frame buffer with heap guard.
//
// CLAUDE.md §6: any allocation ≥1KB must be preceded by a heap guard check.
// Frame buffers at SVGA can be 30–80KB, well above this threshold.
// Rejecting early prevents heap exhaustion from cascading into a crash.
// ---------------------------------------------------------------------------
camera_fb_t* camera_fb_get()
{
    // Frame buffers are allocated in PSRAM by the camera driver, not DRAM.
    // ESP.getFreeHeap() (DRAM) has no bearing on PSRAM availability.
    // ESP.getFreePsram() correctly reflects PSRAM free space.
    // We require at least 64KB free PSRAM for a single SVGA JPEG frame.
    uint32_t free_psram = ESP.getFreePsram();
    if (free_psram < CAMERA_FB_HEAP_GUARD_BYTES) {
        log_e("camera_fb_get: PSRAM low — free=%u, need=%u",
              free_psram, CAMERA_FB_HEAP_GUARD_BYTES);
        return nullptr;
    }

    return esp_camera_fb_get();
}

// ---------------------------------------------------------------------------
// camera_fb_return — Release a frame buffer back to the driver.
//
// Safe to call with nullptr (no-op). Every successful camera_fb_get() must
// have a matching camera_fb_return() to prevent PSRAM leaks.
// ---------------------------------------------------------------------------
void camera_fb_return(camera_fb_t* fb)
{
    if (fb != nullptr) {
        esp_camera_fb_return(fb);
    }
}

// ---------------------------------------------------------------------------
// camera_get_sensor — Get current sensor handle.
//
// For direct register access when the higher-level API doesn't expose a
// needed feature. Use sparingly — prefer camera_set_quality() and the
// typed accessors above for standard operations.
// ---------------------------------------------------------------------------
sensor_t* camera_get_sensor()
{
    return esp_camera_sensor_get();
}

// ---------------------------------------------------------------------------
// Shared "latest frame" snapshot — single-slot, mutex-protected PSRAM buffer.
//
// task_camera publishes into it; the web /snapshot and /stream handlers copy
// out of it. This keeps task_camera the SOLE caller of esp_camera_fb_get()
// (no frame-buffer contention with the RTSP path) while still letting the web
// UI serve a live image.
// ---------------------------------------------------------------------------

static uint8_t*          g_latest_buf = nullptr;   // PSRAM, allocated once
static size_t            g_latest_cap = 0;         // allocated capacity
static size_t            g_latest_len = 0;         // valid bytes in buffer
static uint16_t          g_latest_w   = 0;
static uint16_t          g_latest_h   = 0;
static SemaphoreHandle_t g_latest_mux = nullptr;

bool camera_latest_init(size_t capacity_bytes)
{
    if (g_latest_buf != nullptr) {
        return true;  // already initialized
    }

    g_latest_mux = xSemaphoreCreateMutex();
    if (g_latest_mux == nullptr) {
        log_e("camera_latest_init: failed to create mutex");
        return false;
    }

    // PSRAM-backed: the shared frame lives alongside the driver frame buffers,
    // not in scarce internal DRAM.
    g_latest_buf = (uint8_t*)heap_caps_malloc(capacity_bytes, MALLOC_CAP_SPIRAM);
    if (g_latest_buf == nullptr) {
        log_e("camera_latest_init: failed to allocate %u bytes in PSRAM",
              (unsigned)capacity_bytes);
        vSemaphoreDelete(g_latest_mux);
        g_latest_mux = nullptr;
        return false;
    }

    g_latest_cap = capacity_bytes;
    g_latest_len = 0;
    log_i("camera_latest_init: %u-byte shared frame buffer ready (PSRAM)",
          (unsigned)capacity_bytes);
    return true;
}

void camera_publish_latest(const camera_fb_t* fb)
{
    if (fb == nullptr || fb->buf == nullptr || fb->len == 0) return;
    if (g_latest_buf == nullptr || g_latest_mux == nullptr) return;
    if (fb->len > g_latest_cap) return;  // frame too large for the buffer — skip

    // Short timeout: publishing must never stall the capture task. If a reader
    // is momentarily holding the lock, just skip publishing this frame.
    if (xSemaphoreTake(g_latest_mux, pdMS_TO_TICKS(5)) == pdTRUE) {
        memcpy(g_latest_buf, fb->buf, fb->len);
        g_latest_len = fb->len;
        g_latest_w   = fb->width;
        g_latest_h   = fb->height;
        xSemaphoreGive(g_latest_mux);
    }
}

size_t camera_latest_copy(uint8_t* dst, size_t dst_cap, uint16_t* w, uint16_t* h)
{
    if (dst == nullptr || g_latest_buf == nullptr || g_latest_mux == nullptr) return 0;

    size_t copied = 0;
    if (xSemaphoreTake(g_latest_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (g_latest_len > 0 && g_latest_len <= dst_cap) {
            memcpy(dst, g_latest_buf, g_latest_len);
            copied = g_latest_len;
            if (w) *w = g_latest_w;
            if (h) *h = g_latest_h;
        }
        xSemaphoreGive(g_latest_mux);
    }
    return copied;
}

size_t camera_latest_len()
{
    if (g_latest_buf == nullptr || g_latest_mux == nullptr) return 0;

    size_t len = 0;
    if (xSemaphoreTake(g_latest_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
        len = g_latest_len;
        xSemaphoreGive(g_latest_mux);
    }
    return len;
}
