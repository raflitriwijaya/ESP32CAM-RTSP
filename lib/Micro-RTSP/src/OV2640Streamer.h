#pragma once

#include "CStreamer.h"
#include "OV2640.h"

class OV2640Streamer : public CStreamer
{
    bool m_showBig;
    OV2640 &m_cam;

public:
    OV2640Streamer(SOCKET aClient, OV2640 &cam);

    // Dimension-explicit constructor — no hardware access.
    // Used when frame dimensions are known at construction time (e.g., from
    // camera config constants). Avoids cam.run() → esp_camera_fb_get() which
    // would violate TD-7 single-producer architecture.
    OV2640Streamer(SOCKET aClient, OV2640 &cam, int width, int height);

    virtual void    streamImage(uint32_t curMsec);

    // Stream a pre-captured frame (from the producer-consumer queue).
    // Does NOT call m_cam.run() — the caller owns frame buffer lifecycle.
    // curMsec is used as the RTP timestamp (typically millis()).
    // CLAUDE.md TD-7: this bypasses OV2640::run() to avoid frame-buffer
    // races between task_camera (producer) and task_rtsp (consumer).
    void streamPreCaptured(camera_fb_t* fb, uint32_t curMsec);
};
