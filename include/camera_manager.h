// camera_manager.h — Camera initialization, JPEG quality mapping, and frame buffer lifecycle.
//
// CLAUDE.md §3: camera_manager owns camera init, frame buffer lifecycle, and quality mapping.
// The OV2640 JPEG quality register uses range 0–63 where LOWER = HIGHER quality.
// This module maps the UI-facing 1–100 scale to the driver's 0–63 scale.
// CLAUDE.md §1, §12.

#pragma once

#include <esp_camera.h>
#include <esp_err.h>
#include <stdint.h>

// Initialize camera with production defaults.
//
// Populates camera_config_t using CAM_PIN_* values from config.h (AI-Thinker pinout).
// Disables DCW (downscale) for sharpness per audit recommendation.
// Uses PIXFORMAT_JPEG, FRAMESIZE_SVGA (800x600), fb_count=2, PSRAM-backed.
// Applies heap guard before allocation — returns ESP_ERR_NO_MEM if free heap
// is below HEAP_GUARD_THRESHOLD of total heap.
//
// Returns ESP_OK on success.
esp_err_t camera_init();

// Map UI-facing quality (1–100) to OV2640 driver quality (63–0).
//
// UI=100 → driver=0  (highest quality, largest file).
// UI=1   → driver=63 (lowest quality, smallest file).
// Input is clamped to [JPEG_QUALITY_UI_MIN, JPEG_QUALITY_UI_MAX].
//
// Test cases (verify with integer arithmetic):
//   UI=100 → driver = (63 * (100 - 100)) / 100 = 0
//   UI=1   → driver = (63 * (100 -   1)) / 100 = 63
//   UI=50  → driver = (63 * (100 -  50)) / 100 = 31
//   UI=80  → driver = (63 * (100 -  80)) / 100 = 12
uint8_t camera_map_quality(uint8_t ui_quality);

// Set JPEG quality using UI-scale value (1–100). Maps internally via camera_map_quality().
//
// Returns ESP_ERR_INVALID_STATE if camera sensor handle is not available
// (camera not initialized).
// Returns ESP_OK on success.
esp_err_t camera_set_quality(uint8_t ui_quality);

// Acquire a frame buffer with heap guard.
//
// Returns nullptr if ESP.getFreeHeap() is below HEAP_GUARD_THRESHOLD of total heap
// (CLAUDE.md §6: reject/deny operation when free heap < 20% of total).
// Otherwise returns the result of esp_camera_fb_get().
camera_fb_t* camera_fb_get();

// Release a frame buffer back to the driver.
// Safe to call with nullptr (no-op).
void camera_fb_return(camera_fb_t* fb);

// Get the current sensor handle for direct register access.
// Use sparingly — prefer the typed accessor functions above.
// Returns nullptr if camera has not been initialized.
sensor_t* camera_get_sensor();

// ---------------------------------------------------------------------------
// Shared "latest frame" snapshot (CLAUDE.md §3 producer/consumer).
//
// task_camera (the SOLE owner of the camera driver) publishes each captured
// JPEG into a mutex-protected PSRAM buffer. Consumers that are NOT task_camera
// — specifically the web /snapshot and /stream handlers running in the
// AsyncTCP context — read frames from here instead of calling
// esp_camera_fb_get(), so they never contend with the RTSP path for the two
// driver frame buffers.
// ---------------------------------------------------------------------------

// Allocate the shared buffer (capacity_bytes, PSRAM) and its mutex. Call once,
// after camera_init(). Returns true on success; safe to call more than once.
bool camera_latest_init(size_t capacity_bytes);

// Publish a freshly-captured frame (called ONLY by task_camera). Copies
// fb->buf into the shared buffer under the mutex. Uses a short mutex timeout so
// it never stalls capture — if the lock is momentarily held by a reader, this
// frame's publish is skipped. No-op if not initialized or fb is too large.
void camera_publish_latest(const camera_fb_t* fb);

// Copy the most-recently-published frame into caller storage under the mutex.
// Returns bytes copied (0 if none available or dst too small). w/h may be null.
size_t camera_latest_copy(uint8_t* dst, size_t dst_cap, uint16_t* w, uint16_t* h);

// Length of the latest published frame (0 if none). Use to size a copy buffer.
size_t camera_latest_len();
