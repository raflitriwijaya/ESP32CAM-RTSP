// task_rtsp.cpp — RTSP server task: consumes pre-captured frames from the queue
// and broadcasts them to streaming clients.
//
// CLAUDE.md §5: pinned to core 0, priority 2, 6144-word stack.
// CLAUDE.md §6: max 3 concurrent RTSP clients (hard cap) — enforced inside
//   rtsp_server::client_handler via rtsp_client_accept() + heap guard.
//
// CLAUDE.md TD-7: single-producer, single-consumer architecture.
// task_camera is the ONLY task that calls camera_fb_get() / camera_fb_return().
// This task receives camera_fb_t* pointers from g_frame_queue, broadcasts each
// frame to all streaming RTSP clients via rtsp_server::broadcastFrame(), and
// then returns the buffer via camera_fb_return().
//
// The OV2640 instance is retained ONLY for the rtsp_server constructor and
// OV2640Streamer constructor (dimension discovery). During steady-state
// streaming, OV2640::run() is NEVER called — the internal frame acquisition
// path (OV2640Streamer::streamImage → OV2640::run → esp_camera_fb_get) is
// fully bypassed. All frame data flows through the queue.
//
// Frame data flow:
//   task_camera: camera_fb_get() → xQueueSend(g_frame_queue, &fb)
//   task_rtsp:   xQueueReceive(g_frame_queue, &fb) → server.broadcastFrame(fb)
//                → camera_fb_return(fb)
//
// Buffer lifetime: a frame buffer is acquired by task_camera, transferred
// through the queue, read by all RTSP clients synchronously within
// broadcastFrame(), and then returned to the driver. At no point is a buffer
// returned while still being read.

#include "task_definitions.h"
#include "config.h"
#include "settings.h"
#include "camera_manager.h"
#include "network_manager.h"

#include <freertos/queue.h>
#include <esp_task_wdt.h>
#include <esp32-hal-log.h>
#include <rtsp_server.h>
#include <OV2640.h>

// ---------------------------------------------------------------------------
// External declarations — defined in task_camera.cpp
// ---------------------------------------------------------------------------
extern QueueHandle_t g_frame_queue;

void task_rtsp(void *pvParameters) {
    // -------------------------------------------------------------------
    // One-time initialization: create the OV2640 wrapper and RTSP server.
    //
    // Both are stack-allocated as static to avoid large stack usage in
    // this task (rtsp_server subclasses WiFiServer and contains a
    // std::list, Timer, etc.). They live for the lifetime of the task.
    //
    // The OV2640 instance is retained for dimension discovery only:
    //   - The rtsp_server constructor stores a reference (cam_) for passing
    //     to OV2640Streamer constructors when new clients connect.
    //   - OV2640Streamer's constructor queries cam.getWidth()/getHeight()
    //     which calls runIfNeeded(), acquiring a frame for dimension discovery.
    //     The frame buffer is returned to the pool via cam_.run() in
    //     client_handler, before pushing the new client.
    //   - During streaming, OV2640::run() is NEVER called — frame data comes
    //     from g_frame_queue via task_camera.
    //
    // The rtsp_server constructor calls WiFiServer::begin() which opens a
    // listening socket. This requires the LWIP TCP/IP stack to be running,
    // which is initialized by IotWebConf.init() inside network_init().
    // task_network calls network_init() at startup; we must block until it
    // completes to avoid the LWIP assert at tcpip.c:455.
    //
    // If network_init() fails (never sets the ready flag), we block here
    // and TWDT will reboot the device after 10s. This is intentional —
    // no LWIP means no RTSP, and a reboot gives the system another chance.
    //
    // CLAUDE.md TD-7: OV2640 is NOT used for frame acquisition — task_camera
    // is the sole owner of esp_camera_fb_get() / esp_camera_fb_return().
    // -------------------------------------------------------------------
    log_i("task_rtsp: waiting for network stack (LWIP) to be ready...");
    while (!network_is_ready()) {
        // Reset TWDT while waiting — this task was registered by health_init()
        // in setup(), and the 10s timeout must not fire during normal startup.
        // network_init() typically completes in <1s; this loop is a safety net.
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(100));  // Poll every 100ms, yield to other tasks
    }
    log_i("task_rtsp: network stack ready — constructing RTSP server");

    static OV2640 cam;

    // Derive the RTSP streamer's advertised frame dimensions from the ACTUAL
    // configured frame size — works with ANY size selected via the web UI —
    // instead of the hardcoded VGA fallback constants (which produced the
    // misleading "Created streamer width=640, height=480" log). network_is_ready()
    // (awaited above) guarantees IotWebConf has loaded NVS, so
    // network_get_frame_size() reflects the user's selection (e.g. SVGA 800x600).
    // resolution[] is the esp32-camera driver's framesize -> {width,height} table.
    // (OV2640Streamer::streamPreCaptured() additionally sets m_width/m_height from
    // each fb, so the live RTP dimensions always match the real frame; deriving
    // the initial value here just makes the log and pre-first-frame state correct.)
    framesize_t cfg_fs = network_get_frame_size();
    int frame_w = (cfg_fs < FRAMESIZE_INVALID) ? resolution[cfg_fs].width  : CAMERA_FRAME_WIDTH;
    int frame_h = (cfg_fs < FRAMESIZE_INVALID) ? resolution[cfg_fs].height : CAMERA_FRAME_HEIGHT;
    log_i("task_rtsp: configured frame size = %dx%d (framesize_t=%d)",
          frame_w, frame_h, (int)cfg_fs);

    static rtsp_server server(cam, RTSP_SERVER_TICK_INTERVAL_MS, RTSP_PORT,
                              frame_w, frame_h);

    log_i("task_rtsp: RTSP server listening on port %d, tick_interval=%lu ms, "
          "loop_interval=%lu ms, queue_length=%d, client_cap=%d",
          RTSP_PORT, (unsigned long)RTSP_SERVER_TICK_INTERVAL_MS,
          (unsigned long)RTSP_LOOP_INTERVAL_MS,
          CAMERA_FRAME_QUEUE_LENGTH, MAX_RTSP_CLIENTS);

#ifdef DEBUG
    static uint32_t dbg_frames_received = 0;   // DEBUG: frames consumed from queue
    static uint32_t dbg_frames_broadcast = 0;  // DEBUG: frames broadcast to clients
#endif

    // Sanity check: g_frame_queue must exist before we enter the loop.
    // task_camera creates it during its own initialization. In main.cpp,
    // task_camera is created BEFORE task_rtsp to guarantee this ordering.
    if (g_frame_queue == NULL) {
        log_e("task_rtsp: g_frame_queue is NULL — task_camera must be created "
              "before task_rtsp. Suspending task.");
        vTaskSuspend(NULL);
        return;
    }

    // -------------------------------------------------------------------
    // Steady-state loop — drive the server, check for frames, broadcast.
    //
    // CLAUDE.md §5: no blocking calls inside any task's steady-state loop
    // without an explicit vTaskDelay/yield.
    //
    // Loop structure:
    //   1. Reset TWDT
    //   2. Drive the RTSP server's internal timer (accept clients, handle
    //      RTSP protocol requests, cleanup disconnected clients)
    //   3. Non-blocking check for a new frame from task_camera
    //   4. If a frame is available, broadcast it to all streaming clients
    //      and return the buffer to the driver
    //   5. Yield to other tasks on this core
    //
    // The xQueueReceive timeout is 0 (non-blocking) so the RTSP protocol
    // stays responsive even when no frames are being produced (e.g., before
    // the first client connects and sends PLAY).
    // -------------------------------------------------------------------
    // Timestamp of the last frame actually broadcast — used to pace RTP output
    // to the configured frame rate (see the pacing gate in the loop below).
    uint32_t last_broadcast_ms = 0;

    while (1) {
        // Reset TWDT every loop iteration.
        // CLAUDE.md §8: every long-running task must reset TWDT within
        // the 10-second timeout window.
        esp_task_wdt_reset();

        // Drive the RTSP server's internal timer.
        // doLoop() is non-blocking: it calls timer_.tick() which checks
        // whether any timer callbacks are due and dispatches them inline.
        // The callback (client_handler) handles:
        //   1. Accept new clients (with cap enforcement + heap guard)
        //   2. Service existing clients (handleRequests only — frame
        //      broadcast is handled below)
        //   3. Clean up disconnected clients (release RTSP slots)
        server.doLoop();

        // Check for a new frame from task_camera (non-blocking).
        // If the queue is empty, skip and loop — we don't want to block
        // the RTSP protocol handling waiting for frames.
        camera_fb_t* fb = NULL;
        if (xQueueReceive(g_frame_queue, &fb, 0) == pdTRUE) {
            // PACING (fixes UDP flood → ENOMEM/errno-12 drops → VLC lag/stutter):
            // task_camera captures at ~25 fps, but broadcasting every frame as
            // UDP RTP (an SVGA frame ≈ 35 × 1100-byte fragments) sends ~900
            // packets/sec — far more than the ESP32 WiFi/LWIP TX path can drain,
            // so sendto() fails with ENOMEM and fragments are lost. Broadcast only
            // at the user-configured frame duration ("fd", default 200 ms = 5 fps),
            // always sending the freshest frame and dropping the intermediate ones.
            // This matches the original firmware's cadence and keeps the packet
            // rate sustainable. Read the interval each iteration so web-UI changes
            // to "fd" take effect live.
            uint32_t now = millis();
            uint32_t frame_interval_ms = network_get_frame_duration_ms();
            if (now - last_broadcast_ms >= frame_interval_ms) {
                last_broadcast_ms = now;

                // Broadcast the pre-captured frame to all streaming clients.
                // broadcastFrame() calls OV2640Streamer::streamPreCaptured()
                // which bypasses OV2640::run() entirely — the frame data is
                // read directly from fb->buf / fb->len.
                server.broadcastFrame(fb);

                // Reset TWDT after potentially slow broadcast (socket writes
                // to multiple RTSP clients over WiFi).
                esp_task_wdt_reset();

#ifdef DEBUG
                if (server.num_connected() > 0) dbg_frames_broadcast++;
#endif
            }

            // Return the frame buffer to the driver — whether it was broadcast
            // or dropped by the pacing gate above.
            // CLAUDE.md TD-7: task_rtsp is responsible for returning frames
            // that it received from the queue. At this point all clients
            // have finished reading (broadcastFrame is synchronous), so it
            // is safe to return the buffer.
            camera_fb_return(fb);

#ifdef DEBUG
            dbg_frames_received++;
            if (dbg_frames_received % 100 == 0) {
                log_i("DEBUG task_rtsp: received=%lu, broadcast=%lu, clients=%d",
                      (unsigned long)dbg_frames_received,
                      (unsigned long)dbg_frames_broadcast,
                      (int)server.num_connected());
            }
#endif
        }

        // Yield to other tasks on this core.
        // RTSP_LOOP_INTERVAL_MS (40ms) is shorter than the server's
        // internal tick interval (200ms) so timer checks happen promptly
        // without busy-waiting. This also determines how quickly new frames
        // are picked up from the queue (must be <= CAMERA_CAPTURE_INTERVAL_MS
        // to avoid queue buildup).
        vTaskDelay(pdMS_TO_TICKS(RTSP_LOOP_INTERVAL_MS));
    }
}
