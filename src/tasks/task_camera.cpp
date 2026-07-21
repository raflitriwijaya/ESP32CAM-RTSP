// task_camera.cpp — Sole producer of camera frame buffers.
//
// CLAUDE.md §5: pinned to core 1, priority 3 (highest app priority on its core).
// CLAUDE.md §6: PSRAM-backed frame buffers managed exclusively here.
// Frame cadence must never be stalled by UI or network tasks.
//
// CLAUDE.md TD-7: single-producer, single-consumer architecture.
// task_camera is the ONLY task that calls camera_fb_get() / camera_fb_return().
// It captures frames and sends camera_fb_t* pointers to task_rtsp via a
// FreeRTOS queue (g_frame_queue). task_rtsp broadcasts each frame to RTSP
// clients and is responsible for returning the buffer after broadcast.
//
// Queue depth is CAMERA_FRAME_QUEUE_LENGTH (2), matching fb_count=2.
// If the queue is full, this task blocks for CAMERA_QUEUE_SEND_TIMEOUT_MS (100ms)
// — acceptable backpressure when the RTSP consumer is slower than capture.

#include "task_definitions.h"
#include "config.h"
#include "camera_manager.h"

#include <freertos/queue.h>
#include <esp_task_wdt.h>
#include <esp32-hal-log.h>

// ---------------------------------------------------------------------------
// Global frame queue — created by task_camera, consumed by task_rtsp.
// Holds camera_fb_t* pointers. Queue length matches fb_count (2) so that
// at most 2 frames are in flight at any time.
// ---------------------------------------------------------------------------
QueueHandle_t g_frame_queue = NULL;

void task_camera(void *pvParameters) {
    // -------------------------------------------------------------------
    // Create the frame queue. Must succeed — without it, RTSP streaming
    // has no path to receive frames. If creation fails, log the error and
    // suspend this task permanently (consumer will also fail safely).
    // -------------------------------------------------------------------
    g_frame_queue = xQueueCreate(CAMERA_FRAME_QUEUE_LENGTH, sizeof(camera_fb_t*));
    if (g_frame_queue == NULL) {
        log_e("task_camera: xQueueCreate failed — frame queue is NULL. "
              "RTSP streaming will not function. Suspending task.");
        vTaskSuspend(NULL);
        // Should never reach here — vTaskSuspend blocks indefinitely.
        return;
    }

    log_i("task_camera: frame queue created (length=%d, element_size=%d bytes). "
          "Capture interval=%lu ms, send timeout=%lu ms",
          CAMERA_FRAME_QUEUE_LENGTH, (int)sizeof(camera_fb_t*),
          (unsigned long)CAMERA_CAPTURE_INTERVAL_MS,
          (unsigned long)CAMERA_QUEUE_SEND_TIMEOUT_MS);

    // Allocate the shared "latest frame" buffer so the web /snapshot and
    // /stream handlers can serve frames without calling esp_camera_fb_get()
    // (CLAUDE.md §3: task_camera stays the sole owner of the camera driver).
    // If this fails (PSRAM low), publishing is simply a no-op and the web
    // snapshot returns 503 — RTSP streaming is unaffected.
    if (!camera_latest_init(CAMERA_LATEST_FRAME_BYTES)) {
        log_w("task_camera: shared latest-frame buffer unavailable — "
              "web /snapshot and /stream will be disabled");
    }

#ifdef DEBUG
    static uint32_t dbg_frame_count = 0;    // DEBUG: total frames captured
    static uint32_t dbg_drop_count = 0;     // DEBUG: frames dropped (queue full)
    static uint32_t dbg_null_count = 0;     // DEBUG: camera_fb_get() returned NULL
#endif

    // -------------------------------------------------------------------
    // Steady-state loop — capture frames and dispatch to queue.
    //
    // CLAUDE.md §5: no blocking calls without explicit vTaskDelay/yield.
    // camera_fb_get() may block briefly waiting for a free buffer slot,
    // but this is bounded by the sensor's frame rate (hardware-level,
    // well under the 10-second TWDT timeout).
    //
    // Backpressure: if the queue is full (both slots occupied by frames
    // that task_rtsp hasn't consumed yet), xQueueSend blocks for up to
    // CAMERA_QUEUE_SEND_TIMEOUT_MS. If the timeout expires and the queue
    // is still full, the captured frame is returned to the driver immediately
    // (dropped) — we always prefer a fresh frame over a backlog of stale ones.
    // -------------------------------------------------------------------
    while (1) {
        // Reset TWDT every loop iteration.
        // CLAUDE.md §8: every long-running task must reset TWDT within
        // the 10-second timeout window. Frame capture is fast — well within
        // budget.
        esp_task_wdt_reset();

        // Acquire a frame buffer with heap guard.
        // camera_fb_get() checks free heap before calling esp_camera_fb_get().
        // Returns nullptr if heap is critically low.
        camera_fb_t* fb = camera_fb_get();
        if (fb != NULL) {
#ifdef STREAM_DEBUG
            // TEMPORARY diagnostic — remove with the -DSTREAM_DEBUG flag once verified.
            // Proves the frame captured by the producer is a valid JPEG
            // (first bytes should be FF D8 FF ...) at the configured resolution.
            {
                static uint32_t sd_cap = 0;
                if (sd_cap < 10 && fb->len >= 16) {
                    const uint8_t* b = fb->buf;
                    log_i("STREAM_DEBUG capture: %ux%u len=%u  first16="
                          "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                          fb->width, fb->height, (unsigned)fb->len,
                          b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                          b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
                    sd_cap++;
                }
            }
#endif
            // Publish this frame to the shared latest-frame buffer for the web
            // /snapshot and /stream handlers (copies the bytes; fb ownership is
            // unaffected). Done before xQueueSend, while we still hold fb.
            camera_publish_latest(fb);

            // Send the frame pointer to the RTSP consumer task.
            // Blocks up to CAMERA_QUEUE_SEND_TIMEOUT_MS if queue is full.
            if (xQueueSend(g_frame_queue, &fb, pdMS_TO_TICKS(CAMERA_QUEUE_SEND_TIMEOUT_MS)) != pdTRUE) {
                // Queue is full and timeout expired — consumer is too slow.
                // Return the frame to the driver to prevent buffer starvation.
                // CAMERA_GRAB_LATEST ensures the sensor drops old frames;
                // we just need to keep the 2 buffers circulating.
                log_w("task_camera: queue full for %lu ms — dropping frame (consumer too slow)",
                      (unsigned long)CAMERA_QUEUE_SEND_TIMEOUT_MS);
                camera_fb_return(fb);
#ifdef DEBUG
                dbg_drop_count++;
#endif
            }
#ifdef DEBUG
            else {
                dbg_frame_count++;
                if (dbg_frame_count % 100 == 0) {
                    log_i("DEBUG task_camera: frames_sent=%lu, dropped=%lu, null=%lu",
                          (unsigned long)dbg_frame_count,
                          (unsigned long)dbg_drop_count,
                          (unsigned long)dbg_null_count);
                }
            }
#endif
        } else {
            // camera_fb_get() returned nullptr — either heap guard tripped
            // or the camera driver failed. Log and continue; the next
            // iteration will retry.
            log_w("task_camera: camera_fb_get() returned NULL "
                  "(heap guard tripped or driver error)");
#ifdef DEBUG
            dbg_null_count++;
#endif
        }

        // Yield for the capture interval.
        // CAMERA_CAPTURE_INTERVAL_MS (40ms) targets ~25 fps, which is
        // realistic for SVGA JPEG on OV2640 at moderate quality.
        vTaskDelay(pdMS_TO_TICKS(CAMERA_CAPTURE_INTERVAL_MS));
    }
}
