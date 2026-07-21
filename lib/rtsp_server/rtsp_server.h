#pragma once

#include <list>
#include <WiFiServer.h>
#include <ESPmDNS.h>
#include <OV2640.h>
#include <CRtspSession.h>
#include <arduino-timer.h>

class rtsp_server : public WiFiServer
{
public:
	rtsp_server(OV2640 &cam, unsigned long interval, int port, int frame_width, int frame_height);

	void doLoop();

	// Broadcast a pre-captured frame to all streaming RTSP clients.
	// The caller (task_rtsp) owns frame buffer lifecycle — this method does
	// NOT call esp_camera_fb_return(). The caller must free the buffer after
	// broadcast completes.
	// CLAUDE.md TD-7: single-producer, multi-consumer frame dispatch.
	void broadcastFrame(camera_fb_t* fb);

	size_t num_connected();

private:
	struct rtsp_client
	{
	public:
		rtsp_client(const WiFiClient &client, OV2640 &cam, int width, int height);

		WiFiClient wifi_client;
		// Streamer for UDP/TCP based RTP transport
		std::shared_ptr<CStreamer> streamer;
		// RTSP session and state
		std::shared_ptr<CRtspSession> session;
	};

	OV2640 cam_;
	int frame_width_;
	int frame_height_;
	std::list<std::unique_ptr<rtsp_client>> clients_;
	uintptr_t task_;
	Timer<> timer_;

	static bool client_handler(void *);
};