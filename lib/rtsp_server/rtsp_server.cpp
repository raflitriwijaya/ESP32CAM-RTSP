// rtsp_server.cpp — RTSP server (WiFiServer subclass) with client cap enforcement.

#include "rtsp_server.h"
#include <esp32-hal-log.h>
#include <OV2640Streamer.h>
#include "health_monitor.h"
#include "config.h"

rtsp_server::rtsp_server(OV2640 &cam, unsigned long interval, int port, int frame_width, int frame_height)
	: WiFiServer(port), cam_(cam), frame_width_(frame_width), frame_height_(frame_height)
{
	log_i("Starting RTSP server (frame %dx%d)", frame_width, frame_height);
	WiFiServer::begin();
	timer_.every(interval, client_handler, this);
}

size_t rtsp_server::num_connected()
{
	return clients_.size();
}

void rtsp_server::doLoop()
{
	timer_.tick();
}

rtsp_server::rtsp_client::rtsp_client(const WiFiClient &client, OV2640 &cam, int width, int height)
    : wifi_client(client)
{
	// FIX: use dimension-explicit OV2640Streamer constructor.
	// No cam.run() — no esp_camera_fb_get() — no hardware access.
	// Dimensions are provided from camera config constants.
	// CLAUDE.md TD-7: task_camera is sole owner of esp_camera_fb_get().
	streamer = std::shared_ptr<OV2640Streamer>(new OV2640Streamer(&wifi_client, cam, width, height));
	session = std::shared_ptr<CRtspSession>(new CRtspSession(&wifi_client, streamer.get()));
}

void rtsp_server::broadcastFrame(camera_fb_t* fb)
{
	if (!fb || !fb->buf || fb->len == 0) return;
	uint32_t now = millis();
	for (const auto &client : clients_) {
		if (client->session->m_streaming && !client->session->m_stopped) {
			auto* s = static_cast<OV2640Streamer*>(client->streamer.get());
			// FIX: use streamPreCaptured() — the frame was already captured by
			// task_camera and passed through g_frame_queue. streamImage() would
			// call m_cam.run() → esp_camera_fb_get() which deadlocks when all
			// buffers are held by task_camera/queue (TWDT trigger root cause).
			// CLAUDE.md TD-7: task_camera is sole owner of esp_camera_fb_get().
			s->streamPreCaptured(fb, now);
		}
	}
}

bool rtsp_server::client_handler(void *arg)
{
	auto self = static_cast<rtsp_server *>(arg);
	auto new_client = self->accept();
	if (new_client) {
		if (rtsp_client_accept()) {
			if (!heap_can_allocate(12000)) {
				log_e("RTSP: heap guard tripped");
				new_client.println("RTSP/1.0 503 Service Unavailable");
				new_client.println("CSeq: 0");
				new_client.stop();
				rtsp_client_release();
				return true;
			}

			// FIX: no esp_camera_fb_get() or cam.run() calls here.
			// Dimensions are passed from rtsp_server's stored config values.
			// The OV2640Streamer uses the dimension-explicit constructor
			// which avoids all camera hardware access.
			// CLAUDE.md TD-7: task_camera is sole owner of esp_camera_fb_get().
			if (!esp_camera_sensor_get()) {
				log_e("RTSP: camera not initialized — rejecting");
				new_client.println("RTSP/1.0 503 Service Unavailable");
				new_client.println("CSeq: 0");
				new_client.stop();
				rtsp_client_release();
				return true;
			}

			self->clients_.push_back(
				std::unique_ptr<rtsp_client>(
					new rtsp_client(new_client, self->cam_,
					                self->frame_width_, self->frame_height_)));

			log_i("RTSP: client accepted — total=%d, cap=%d",
			      health_get_rtsp_client_count(), MAX_RTSP_CLIENTS);
		} else {
			log_w("RTSP: client cap exceeded (%d/%d) — sending 503",
			      health_get_rtsp_client_count(), MAX_RTSP_CLIENTS);
			new_client.println("RTSP/1.0 503 Service Unavailable");
			new_client.println("CSeq: 0");
			new_client.println("Server: ESP32-Cam-RTSP");
			new_client.stop();
		}
	}

	for (const auto &client : self->clients_) {
		client->session->handleRequests(0);
	}

	self->clients_.remove_if([](std::unique_ptr<rtsp_client> const &c) {
		if (c->session->m_stopped) {
			rtsp_client_release();
			log_i("RTSP: client disconnected — total=%d, cap=%d",
			      health_get_rtsp_client_count(), MAX_RTSP_CLIENTS);
			return true;
		}
		return false;
	});

	return true;
}
