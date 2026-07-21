
#include "OV2640Streamer.h"
#include <assert.h>



OV2640Streamer::OV2640Streamer(SOCKET aClient, OV2640 &cam) : CStreamer(aClient, 0, 0), m_cam(cam)
{
    // FIXME(VENDOR): These files should be vendored to lib/Micro-RTSP/ in a
    // later refactor step. For now, modified in-place in the managed dependency.
    //
    // Explicitly grab the first frame before reading dimensions.
    // The original code called cam.getWidth()/getHeight() in the initializer
    // list, which triggered an implicit runIfNeeded() → run() inside getWidth().
    // That worked but was race-prone (AUDIT_REPORT_V1.md §2). Making run()
    // explicit ensures the frame is ready before we read its dimensions.
    m_cam.run();
    m_width  = m_cam.getWidth();
    m_height = m_cam.getHeight();
    printf("Created streamer width=%d, height=%d\n", m_width, m_height);
}

OV2640Streamer::OV2640Streamer(SOCKET aClient, OV2640 &cam, int width, int height)
    : CStreamer(aClient, width, height), m_cam(cam)
{
    // Dimension-explicit constructor — no hardware access.
    // The caller provides frame dimensions from camera config constants,
    // avoiding cam.run() → esp_camera_fb_get() entirely.
    // CLAUDE.md TD-7: task_camera is sole owner of esp_camera_fb_get().
    m_width  = width;
    m_height = height;
    printf("Created streamer width=%d, height=%d (dimensions provided)\n", m_width, m_height);
}

void OV2640Streamer::streamImage(uint32_t curMsec)
{
    m_cam.run();// queue up a read for next time

    BufPtr bytes = m_cam.getfb();
    streamFrame(bytes, m_cam.getSize(), curMsec);
}

void OV2640Streamer::streamPreCaptured(camera_fb_t* fb, uint32_t curMsec)
{
    // Bypass m_cam.run() entirely — the frame was captured by task_camera
    // and passed through the FreeRTOS queue. The caller (task_rtsp) owns
    // frame buffer lifecycle and will call camera_fb_return() after all
    // clients have broadcast this frame.
    // CLAUDE.md TD-7: single-producer (task_camera), multi-consumer (RTSP
    // clients reading the same buffer) — the buffer is NOT freed here.
    if (fb && fb->buf && fb->len > 0) {
        m_width = fb->width;
        m_height = fb->height;
#ifdef STREAM_DEBUG
        // TEMPORARY diagnostic — remove with the -DSTREAM_DEBUG flag once verified.
        // Proves the ACTUAL dimensions and JPEG magic (should start FF D8 FF)
        // that reach the streamer, independent of the constructor's log line.
        {
            static uint32_t sd_str = 0;
            if (sd_str < 10 && fb->len >= 16) {
                const unsigned char *b = fb->buf;
                printf("STREAM_DEBUG streamer: %ux%u len=%u  first16="
                       "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                       (unsigned)fb->width, (unsigned)fb->height, (unsigned)fb->len,
                       b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                       b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
                sd_str++;
            }
        }
#endif
        streamFrame(fb->buf, fb->len, curMsec);
    }
}
