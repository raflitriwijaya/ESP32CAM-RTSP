// web_ui_service.cpp — Async HTTP server, route handlers, template caching.
//
// CLAUDE.md §7.1: ESPAsyncWebServer + AsyncTCP for all HTTP request handling.
// IotWebConf is scoped strictly to provisioning (captive portal, NVS config).
// moustache_render() output must be cached — re-render only when backing
// parameters change.
//
// CLAUDE.md §6: HTTP client cap (MAX_HTTP_CLIENTS=5) enforced at handler entry
// via http_client_accept(). Release via AsyncClient::onDisconnect callback
// so the counter tracks actual TCP connection lifetime, not just handler duration.
//
// CLAUDE.md §7.1 integration rule: IotWebConf config portal runs on
// IOTWEBCONF_WEB_PORT (8080). AsyncWebServer on port 80 handles the main web UI.
// In AP/provisioning mode, port 80 redirects to IotWebConf's captive portal.

#include "web_ui_service.h"
#include "network_manager.h"
#include "health_monitor.h"
#include "camera_manager.h"
#include "config.h"
#include "settings.h"
#include "task_definitions.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <ESPAsyncWebServer.h>
#include <moustache.h>
#include <memory>

// Lookup tables for reverse-mapping sensor values → human-readable names
// (the existing lookup_*.h headers go name→value; we need value→name here).
#include <lookup_camera_effect.h>
#include <lookup_camera_frame_size.h>
#include <lookup_camera_gainceiling.h>
#include <lookup_camera_wb_mode.h>

// ---------------------------------------------------------------------------
// Externs — symbols defined elsewhere that we need read access to.
// ---------------------------------------------------------------------------

// Embedded HTML template (minified) — linked from build via board_build.embed_txtfiles.
// PlatformIO generates _binary_ prefix symbols for embedded files.
extern const uint8_t index_min_html_start[] asm("_binary_html_index_min_html_start");
extern const uint8_t index_min_html_end[]   asm("_binary_html_index_min_html_end");

// Camera init result from setup() in main.cpp — used for CameraInitialized
// section in the template.
extern esp_err_t camera_init_result;

// ---------------------------------------------------------------------------
// Template Variable Count — must match the moustache variable array size.
// Update this constant if the HTML template gains or loses variables.
// Current count derived from html/index.min.html audit (2026-07-08).
// ---------------------------------------------------------------------------
#define TEMPLATE_VAR_COUNT  55

// ---------------------------------------------------------------------------
// File-Scope State
// ---------------------------------------------------------------------------

// AsyncWebServer* — allocated once in web_ui_init().
// CLAUDE.md §4 exception: single pre-sized pool object (one server instance,
// not per-client). Allocation is heap-guarded and happens during task init
// before the steady-state loop, with no further allocations during runtime.
static AsyncWebServer* g_async_server = nullptr;

// Template caching (CLAUDE.md §7.1)
static String  g_cached_template;       // Rendered HTML, ready to serve
static bool    g_cache_valid = false;    // false → needs re-render on next GET /

// Status JSON cache — rebuilt every 2 seconds by task_web_ui
static String  g_cached_status_json;

// ---------------------------------------------------------------------------
// Forward Declarations
// ---------------------------------------------------------------------------

static void on_client_disconnect(void* arg, AsyncClient* client);
static String render_template();
static String build_status_json();

// ---------------------------------------------------------------------------
// Helper: reverse-lookup sensor value → human-readable name.
// The lookup_*.h headers only provide name→value. These functions search
// the constexpr arrays in the reverse direction.
// ---------------------------------------------------------------------------

static const char* lookup_effect_name(int value)
{
    for (const auto& entry : camera_effects) {
        if (entry.value == value) return entry.name;
    }
    return "Unknown";
}

static const char* lookup_frame_size_name(framesize_t fs)
{
    for (const auto& entry : frame_sizes) {
        if (entry.frame_size == fs) return entry.name;
    }
    return "Unknown";
}

static const char* lookup_gain_ceiling_name(gainceiling_t gc)
{
    for (const auto& entry : camera_gain_ceilings) {
        if (entry.value == gc) return entry.name;
    }
    return "Unknown";
}

static const char* lookup_wb_mode_name(int value)
{
    for (const auto& entry : camera_wb_modes) {
        if (entry.value == value) return entry.name;
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// HTTP Client Disconnect Callback
//
// Registered on each accepted client's AsyncClient. Fires when the TCP
// connection closes (response sent, client disconnects, or timeout).
// CLAUDE.md §6: hard cap of MAX_HTTP_CLIENTS concurrent TCP connections.
//
// http_client_release() is safe to call on connections that were never
// accepted (it floors at 0), so a stray callback on a rejected client
// does no harm.
// ---------------------------------------------------------------------------

static void on_client_disconnect(void* arg, AsyncClient* client)
{
    http_client_release();
}

// ---------------------------------------------------------------------------
// Route Handler: handle_root — GET /
//
// Serves the cached HTML template. If the cache is invalid (settings
// changed, WiFi state changed), re-renders before serving.
//
// In AP/provisioning mode: redirects to IotWebConf's captive portal on
// port IOTWEBCONF_WEB_PORT so the user can configure WiFi.
//
// Client cap enforced at entry (CLAUDE.md §6).
// ---------------------------------------------------------------------------

static void handle_root(AsyncWebServerRequest* request)
{
    // --- AP mode captive portal redirect ---
    // In AP/provisioning mode, IotWebConf handles the captive portal on
    // a different port. Redirect the user there so they can configure WiFi.
    if (network_is_ap_mode()) {
        uint16_t config_port = network_get_config_port();
        String redirect_url = "http://";
        redirect_url += WiFi.softAPIP().toString();
        redirect_url += ":";
        redirect_url += config_port;
        redirect_url += "/";
        AsyncWebServerResponse* resp = request->beginResponse(302, "text/plain",
            "Redirecting to configuration portal...");
        resp->addHeader("Location", redirect_url);
        request->send(resp);
        return;
    }

    // --- Client cap enforcement ---
    if (!http_client_accept()) {
        AsyncWebServerResponse* resp = request->beginResponse(503, "text/plain",
            "503 Service Unavailable");
        resp->addHeader("Retry-After", "30");
        request->send(resp);
        return;
    }

    // Register disconnect callback for accurate client tracking.
    AsyncClient* client = request->client();
    if (client != nullptr) {
        client->onDisconnect(on_client_disconnect);
    } else {
        // Client already gone — release immediately.
        http_client_release();
        return;
    }

    // --- Template caching ---
    if (!g_cache_valid) {
        g_cached_template = render_template();
        g_cache_valid = true;
    }

    // Connection: close prevents keep-alive, which ensures each TCP connection
    // maps 1:1 to http_client_accept()/release() pairs (CLAUDE.md §6).
    AsyncWebServerResponse* resp = request->beginResponse(200,
        "text/html; charset=UTF-8", g_cached_template);
    resp->addHeader("Connection", "close");
    request->send(resp);
}

// ---------------------------------------------------------------------------
// Route Handler: handle_status — GET /status
//
// Serves the cached status JSON. The cache is rebuilt every 2 seconds by
// web_ui_update_status() called from task_web_ui.
//
// Client cap enforced at entry (CLAUDE.md §6).
// ---------------------------------------------------------------------------

static void handle_status(AsyncWebServerRequest* request)
{
    if (!http_client_accept()) {
        AsyncWebServerResponse* resp = request->beginResponse(503, "text/plain",
            "503 Service Unavailable");
        resp->addHeader("Retry-After", "30");
        request->send(resp);
        return;
    }

    AsyncClient* client = request->client();
    if (client != nullptr) {
        client->onDisconnect(on_client_disconnect);
    } else {
        http_client_release();
        return;
    }

    // Connection: close prevents keep-alive, ensuring 1:1 accept/release pairs.
    AsyncWebServerResponse* json_resp = request->beginResponse(200,
        "application/json; charset=UTF-8", g_cached_status_json);
    json_resp->addHeader("Connection", "close");
    request->send(json_resp);
}

// ---------------------------------------------------------------------------
// Route Handler: handle_config — GET /config
//
// Redirects to IotWebConf's configuration portal on IOTWEBCONF_WEB_PORT.
// The config form is served by IotWebConf's synchronous WebServer.
// ---------------------------------------------------------------------------

static void handle_config(AsyncWebServerRequest* request)
{
    // --- Client cap enforcement (CLAUDE.md §6) ---
    if (!http_client_accept()) {
        AsyncWebServerResponse* resp = request->beginResponse(503, "text/plain",
            "503 Service Unavailable");
        resp->addHeader("Retry-After", "30");
        request->send(resp);
        return;
    }

    AsyncClient* client = request->client();
    if (client != nullptr) {
        client->onDisconnect(on_client_disconnect);
    } else {
        http_client_release();
        return;
    }

    WiFiStatus status = network_get_status();
    IPAddress ip;

    if (status.state == WiFiState::STA_CONNECTED) {
        ip = WiFi.localIP();
    } else {
        ip = WiFi.softAPIP();
    }

    String redirect_url = "http://";
    redirect_url += ip.toString();
    redirect_url += ":";
    redirect_url += network_get_config_port();
    redirect_url += "/";   // IotWebConf serves config portal on root path

    AsyncWebServerResponse* resp = request->beginResponse(302, "text/plain",
        "Redirecting to configuration portal...");
    resp->addHeader("Location", redirect_url);
    resp->addHeader("Connection", "close");
    request->send(resp);
}

// ---------------------------------------------------------------------------
// Route Handler: handle_snapshot — GET /snapshot
//
// Captures a single JPEG frame from the camera and returns it as image/jpeg.
// Useful for quick camera diagnostics and still-image capture.
// Client cap enforced at entry (CLAUDE.md §6).
// ---------------------------------------------------------------------------

static void handle_snapshot(AsyncWebServerRequest* request)
{
    if (!http_client_accept()) {
        AsyncWebServerResponse* resp = request->beginResponse(503, "text/plain",
            "503 Service Unavailable");
        resp->addHeader("Retry-After", "30");
        request->send(resp);
        return;
    }

    AsyncClient* client = request->client();
    if (client != nullptr) {
        client->onDisconnect(on_client_disconnect);
    } else {
        http_client_release();
        return;
    }

    // Serve the most recent frame published by task_camera — we do NOT call
    // esp_camera_fb_get() here (CLAUDE.md §3: task_camera is the sole camera-
    // driver owner, so the web UI never contends with the RTSP path for the
    // two driver frame buffers). Copy the frame into a PSRAM buffer owned by a
    // shared_ptr captured in the response filler; the buffer is freed
    // automatically when the response completes OR the client disconnects
    // (shared_ptr deleter) — no leak on either path.
    // Note: the client slot is released by on_client_disconnect (registered
    // above) when the TCP connection closes — including for these 503 paths —
    // so we must NOT call http_client_release() here (that would double-count).
    size_t len = camera_latest_len();
    if (len == 0) {
        request->send(503, "text/plain", "Camera frame not available yet");
        return;
    }

    std::shared_ptr<uint8_t> frame(
        (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM),
        [](uint8_t* p) { if (p) heap_caps_free(p); });
    if (!frame) {
        request->send(503, "text/plain", "Out of memory for snapshot");
        return;
    }

    size_t got = camera_latest_copy(frame.get(), len, nullptr, nullptr);
    if (got == 0) {
        request->send(503, "text/plain", "Camera frame not available yet");
        return;
    }

    AsyncWebServerResponse* resp = request->beginResponse("image/jpeg", got,
        [frame, got](uint8_t* out, size_t maxLen, size_t index) -> size_t {
            if (index >= got) return 0;
            size_t remaining = got - index;
            size_t n = (remaining < maxLen) ? remaining : maxLen;
            memcpy(out, frame.get() + index, n);
            return n;
        });
    resp->addHeader("Connection", "close");
    request->send(resp);
}

// ---------------------------------------------------------------------------
// Per-request state for the MJPEG /stream chunked response. Held by a
// shared_ptr captured in the filler lambda, so it (and its PSRAM frame buffer)
// is released automatically when the response ends OR the client disconnects.
// ---------------------------------------------------------------------------
struct MjpegStreamState {
    std::shared_ptr<uint8_t> frame;   // PSRAM copy of the current frame
    size_t frame_len  = 0;            // bytes of JPEG in `frame`
    size_t header_len = 0;            // bytes of the multipart part header
    size_t sent       = 0;            // bytes of the current part already emitted
    bool   have_part  = false;        // is a part currently in progress?
    char   header[96] = {0};          // multipart boundary + per-part headers
};

// ---------------------------------------------------------------------------
// Route Handler: handle_stream — GET /stream
//
// MJPEG over HTTP: continuously sends JPEG frames as multipart/x-mixed-replace,
// fed from the shared latest-frame buffer (no esp_camera_fb_get() here).
// Client cap enforced at entry (CLAUDE.md §6).
// ---------------------------------------------------------------------------

static void handle_stream(AsyncWebServerRequest* request)
{
    if (!http_client_accept()) {
        AsyncWebServerResponse* resp = request->beginResponse(503, "text/plain",
            "503 Service Unavailable");
        resp->addHeader("Retry-After", "30");
        request->send(resp);
        return;
    }

    AsyncClient* client = request->client();
    if (client != nullptr) {
        client->onDisconnect(on_client_disconnect);
    } else {
        http_client_release();
        return;
    }

    // MJPEG streaming fed from the shared latest-frame buffer — NO
    // esp_camera_fb_get() here (CLAUDE.md §3: task_camera is the sole camera-
    // driver owner). A per-request state object (freed via shared_ptr when the
    // response ends or the client disconnects) holds a copy of the freshest
    // published frame and streams it across as many chunks as needed. The old
    // handler returned 0 whenever a frame exceeded one ~1.4 KB chunk, so it
    // never actually streamed an SVGA frame.
    //
    // TCP flow-control self-paces this stream (unlike the RTSP/UDP path it
    // cannot flood the TX buffers), but it does share WiFi airtime with RTSP —
    // running both at once is not recommended.
    auto state = std::make_shared<MjpegStreamState>();
    state->frame = std::shared_ptr<uint8_t>(
        (uint8_t*)heap_caps_malloc(CAMERA_LATEST_FRAME_BYTES, MALLOC_CAP_SPIRAM),
        [](uint8_t* p) { if (p) heap_caps_free(p); });
    if (!state->frame) {
        request->send(503, "text/plain", "Out of memory for stream");
        return;
    }

    AsyncWebServerResponse* resp = request->beginChunkedResponse(
        "multipart/x-mixed-replace; boundary=frame",
        [state](uint8_t* out, size_t maxLen, size_t index) -> size_t {
            // Start a new MJPEG part when the previous one is fully sent.
            if (!state->have_part) {
                size_t got = camera_latest_copy(state->frame.get(),
                                                CAMERA_LATEST_FRAME_BYTES,
                                                nullptr, nullptr);
                if (got == 0) {
                    // No frame yet — emit a harmless CRLF keep-alive so the
                    // chunked stream stays open (returning 0 would end it).
                    if (maxLen >= 2) { out[0] = '\r'; out[1] = '\n'; return 2; }
                    return 0;
                }
                state->frame_len  = got;
                state->header_len = snprintf(state->header, sizeof(state->header),
                    "\r\n--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                    (unsigned)got);
                state->sent = 0;
                state->have_part = true;
            }

            // Emit the next slice of the current part (header bytes, then frame
            // bytes). A single filler call may straddle the header→frame edge.
            size_t part_total = state->header_len + state->frame_len;
            size_t remaining  = part_total - state->sent;
            size_t n = (remaining < maxLen) ? remaining : maxLen;

            size_t written = 0;
            while (written < n) {
                size_t pos = state->sent + written;
                if (pos < state->header_len) {
                    size_t take = state->header_len - pos;
                    if (take > n - written) take = n - written;
                    memcpy(out + written, state->header + pos, take);
                    written += take;
                } else {
                    size_t fpos = pos - state->header_len;
                    size_t take = state->frame_len - fpos;
                    if (take > n - written) take = n - written;
                    memcpy(out + written, state->frame.get() + fpos, take);
                    written += take;
                }
            }

            state->sent += n;
            if (state->sent >= part_total) {
                state->have_part = false;  // part complete → next call grabs a new frame
            }
            return n;
        });
    resp->addHeader("Connection", "close");
    request->send(resp);
}

// ---------------------------------------------------------------------------
// Route Handler: handle_not_found — 404 handler
//
// In AP mode: detect captive portal probes (phones checking connectivity)
// and redirect them to the IotWebConf config portal.
// In STA mode: return a plain 404.
// ---------------------------------------------------------------------------

static void handle_not_found(AsyncWebServerRequest* request)
{
    // --- Client cap enforcement (CLAUDE.md §6) ---
    // Enforced on all handlers: captive portal redirects in AP mode allocate
    // String objects for the redirect URL, and even the trivial 404 path
    // consumes an AsyncTCP connection slot.
    if (!http_client_accept()) {
        AsyncWebServerResponse* resp = request->beginResponse(503, "text/plain",
            "503 Service Unavailable");
        resp->addHeader("Retry-After", "30");
        request->send(resp);
        return;
    }

    AsyncClient* client = request->client();
    if (client != nullptr) {
        client->onDisconnect(on_client_disconnect);
    } else {
        http_client_release();
        return;
    }

    if (network_is_ap_mode()) {
        // Captive portal detection — any unknown path in AP mode
        // is likely a connectivity check. Redirect to config portal.
        IPAddress ap_ip = WiFi.softAPIP();
        String redirect_url = "http://";
        redirect_url += ap_ip.toString();
        redirect_url += ":";
        redirect_url += network_get_config_port();
        redirect_url += "/";

        AsyncWebServerResponse* resp = request->beginResponse(302, "text/plain",
            "Redirecting to configuration portal...");
        resp->addHeader("Location", redirect_url);
        resp->addHeader("Connection", "close");
        request->send(resp);
        return;
    }

    AsyncWebServerResponse* resp = request->beginResponse(404, "text/plain",
        "404 Not Found");
    resp->addHeader("Connection", "close");
    request->send(resp);
}

// ---------------------------------------------------------------------------
// Template Renderer
//
// Reads the embedded HTML template, populates all 55 moustache variables
// from current system state (HAL, camera sensor, network, health), and
// calls moustache_render().
//
// Called when g_cache_valid == false (first request or after invalidation).
// The rendered result is stored in g_cached_template.
//
// CLAUDE.md §7.1: re-renders only when backing parameters change.
// CLAUDE.md §6: if the template string exceeds MAX_TEMPLATE_SIZE_BYTES,
//   truncate with a comment — this indicates a bug in the template.
// ---------------------------------------------------------------------------

static String render_template()
{
    // --- Read embedded template ---
    size_t template_len = index_min_html_end - index_min_html_start;
    String format_str;
    format_str.concat((const char*)index_min_html_start, template_len);

    // --- Gather current state ---
    WiFiStatus wifi = network_get_status();
    sensor_t* sensor = camera_get_sensor();
    bool camera_ok = (sensor != nullptr && camera_init_result == ESP_OK);
    uint32_t frame_dur = network_get_frame_duration_ms();

    // Sensor-register values (read via pointer-based getters on sensor_t).
    // Default to 0/false if sensor is not available.
    int sensor_brightness = 0, sensor_contrast = 0, sensor_saturation = 0;
    int sensor_effect = 0, sensor_whitebal = 0, sensor_awb_gain = 0;
    int sensor_wb_mode_val = 0, sensor_exposure_ctrl = 0, sensor_aec2 = 0;
    int sensor_ae_level = 0, sensor_aec_value = 0, sensor_gain_ctrl = 0;
    int sensor_agc_gain = 0, sensor_bpc = 0, sensor_wpc = 0;
    int sensor_raw_gma = 0, sensor_lenc = 0, sensor_hmirror = 0;
    int sensor_vflip = 0, sensor_dcw = 0, sensor_colorbar = 0;
    int sensor_jpg_quality_driver = 0;  // Raw OV2640 register value (0-63)
    framesize_t sensor_framesize = FRAMESIZE_INVALID;
    gainceiling_t sensor_gain_ceiling = GAINCEILING_2X;

    if (sensor != nullptr) {
        // sensor_t provides only set_* methods, no get_*.
        // Read current values directly from sensor->status (camera_status_t).
        // CLAUDE.md §1: OV2640 driver — sensor->status reflects the last-set values.
        sensor_brightness      = sensor->status.brightness;
        sensor_contrast        = sensor->status.contrast;
        sensor_saturation      = sensor->status.saturation;
        sensor_effect          = sensor->status.special_effect;
        sensor_whitebal        = sensor->status.awb;
        sensor_awb_gain        = sensor->status.awb_gain;
        sensor_wb_mode_val     = sensor->status.wb_mode;
        sensor_exposure_ctrl   = sensor->status.aec;
        sensor_aec2            = sensor->status.aec2;
        sensor_ae_level        = sensor->status.ae_level;
        sensor_aec_value       = sensor->status.aec_value;
        sensor_gain_ctrl       = sensor->status.agc;
        sensor_agc_gain        = sensor->status.agc_gain;
        sensor_gain_ceiling    = static_cast<gainceiling_t>(sensor->status.gainceiling);
        sensor_bpc             = sensor->status.bpc;
        sensor_wpc             = sensor->status.wpc;
        sensor_raw_gma         = sensor->status.raw_gma;
        sensor_lenc            = sensor->status.lenc;
        sensor_hmirror         = sensor->status.hmirror;
        sensor_vflip           = sensor->status.vflip;
        sensor_dcw             = sensor->status.dcw;
        sensor_colorbar        = sensor->status.colorbar;
        sensor_framesize       = sensor->status.framesize;
        sensor_jpg_quality_driver = sensor->status.quality;
    }

    // --- WiFi mode string ---
    const char* wifi_mode_str;
    const char* access_point_str;
    switch (wifi.state) {
    case WiFiState::STA_CONNECTED:
        wifi_mode_str = "STA";
        access_point_str = WiFi.SSID().c_str();
        break;
    case WiFiState::STA_CONNECTING:
        wifi_mode_str = "Connecting...";
        access_point_str = WiFi.SSID().c_str();
        break;
    case WiFiState::AP_MODE:
    case WiFiState::PROVISIONING:
        wifi_mode_str = "AP";
        access_point_str = WiFi.softAPSSID().c_str();
        break;
    case WiFiState::DISCONNECTED:
    default:
        wifi_mode_str = "Disconnected";
        access_point_str = "-";
        break;
    }

    // --- Camera init error text ---
    String camera_err_text;
    String camera_err_code;
    if (camera_init_result == ESP_OK) {
        camera_err_text = "OK";
        camera_err_code = "0";
    } else {
        camera_err_text = "Error 0x";
        camera_err_text += String(camera_init_result, HEX);
        camera_err_code = String((int)camera_init_result);
    }

    // --- Build moustache variable array ---
    // All values are Strings. Boolean sections use "1" (true) / "0" (false).
    // CLAUDE.md §4: no magic numbers — all names must match the template.
    moustache_variable_t vars[TEMPLATE_VAR_COUNT] = {
        // ---- Basic info ----
        {"AppTitle",            APP_TITLE},
        {"AppVersion",          APP_VERSION},
        {"ThingName",           WiFi.getHostname()},
        // ---- ESP32 hardware ----
        {"BoardType",           BOARD_NAME},
        {"SDKVersion",          ESP.getSdkVersion()},
        {"ChipModel",           ESP.getChipModel()},
        {"ChipRevision",        String(ESP.getChipRevision())},
        {"CpuFreqMHz",          String(getCpuFrequencyMhz())},
        {"CpuCores",            String(ESP.getChipCores())},
        {"HeapSize",            String(ESP.getHeapSize())},
        {"PsRamSize",           String(ESP.getPsramSize())},
        {"FlashSize",           String(ESP.getFlashChipSize())},
        // ---- Diagnostics ----
        {"Uptime",              String(wifi.uptime_seconds)},
        {"NumRTSPSessions",     String(health_get_rtsp_client_count())},
        {"FreeHeap",            String(ESP.getFreeHeap())},
        {"MaxAllocHeap",        String(ESP.getMaxAllocHeap())},
        // ---- Network ----
        {"HostName",            WiFi.getHostname()},
        {"MacAddress",          WiFi.macAddress()},
        {"WifiMode",            wifi_mode_str},
        {"AccessPoint",         access_point_str},
        {"SignalStrength",      String(wifi.rssi)},
        {"IPv4",                (wifi.state == WiFiState::STA_CONNECTED)
                                    ? WiFi.localIP().toString()
                                    : WiFi.softAPIP().toString()},
        {"IPv6",                WiFi.localIPv6().toString()},
        // ---- Network state booleans ----
        {"NetworkState.ApMode", (network_is_ap_mode()) ? "1" : "0"},
        {"NetworkState.OnLine", (wifi.state == WiFiState::STA_CONNECTED) ? "1" : "0"},
        // ---- Camera ----
        {"FrameDuration",       String(frame_dur)},
        {"FrameFrequency",      (frame_dur > 0) ? String(1000 / frame_dur) : "0"},
        {"FrameSize",           lookup_frame_size_name(sensor_framesize)},
        // JPEG quality: inverse-map from OV2640 driver value (0-63, lower=higher)
        // to UI value (1-100, higher=better).
        // driver → UI: UI_MAX - (driver * UI_MAX / DRIVER_MAX)
        {"JpegQuality",         String(JPEG_QUALITY_UI_MAX
                                       - (sensor_jpg_quality_driver
                                          * JPEG_QUALITY_UI_MAX
                                          / JPEG_QUALITY_DRIVER_MAX))},
        // ---- Camera sensor values ----
        {"Brightness",          String(sensor_brightness)},
        {"Contrast",            String(sensor_contrast)},
        {"Saturation",          String(sensor_saturation)},
        {"SpecialEffect",       lookup_effect_name(sensor_effect)},
        {"WhiteBal",            sensor_whitebal ? "1" : "0"},
        {"AwbGain",             sensor_awb_gain ? "1" : "0"},
        {"WbMode",              lookup_wb_mode_name(sensor_wb_mode_val)},
        {"ExposureCtrl",        sensor_exposure_ctrl ? "1" : "0"},
        {"Aec2",                sensor_aec2 ? "1" : "0"},
        {"AeLevel",             String(sensor_ae_level)},
        {"AecValue",            String(sensor_aec_value)},
        {"GainCtrl",            sensor_gain_ctrl ? "1" : "0"},
        {"AgcGain",             String(sensor_agc_gain)},
        {"GainCeiling",         lookup_gain_ceiling_name(sensor_gain_ceiling)},
        {"Bpc",                 sensor_bpc ? "1" : "0"},
        {"Wpc",                 sensor_wpc ? "1" : "0"},
        {"RawGma",              sensor_raw_gma ? "1" : "0"},
        {"Lenc",                sensor_lenc ? "1" : "0"},
        {"HMirror",             sensor_hmirror ? "1" : "0"},
        {"VFlip",               sensor_vflip ? "1" : "0"},
        {"Dcw",                 sensor_dcw ? "1" : "0"},
        {"ColorBar",            sensor_colorbar ? "1" : "0"},
        // ---- Camera initialization status ----
        {"CameraInitialized",       camera_ok ? "1" : "0"},
        {"CameraInitResult",        camera_err_code},
        {"CameraInitResultText",    camera_err_text},
        // ---- RTSP ----
        {"RtspPort",            String(RTSP_PORT)},
    };

    // --- Render with moustache ---
    String rendered = moustache_render(format_str, vars);

    // --- Size guard (CLAUDE.md §6, §7.1) ---
    if (rendered.length() > MAX_TEMPLATE_SIZE_BYTES) {
        log_e("render_template: rendered HTML (%u bytes) exceeds "
              "MAX_TEMPLATE_SIZE_BYTES (%u) — truncating",
              rendered.length(), MAX_TEMPLATE_SIZE_BYTES);
        rendered = rendered.substring(0, MAX_TEMPLATE_SIZE_BYTES);
        rendered += "\n<!-- TRUNCATED — exceeded MAX_TEMPLATE_SIZE_BYTES -->";
    }

    return rendered;
}

// ---------------------------------------------------------------------------
// Status JSON Builder
//
// Builds the /status JSON payload from current system state.
// Called by web_ui_update_status() every ~2 seconds.
// Uses snprintf for minimal heap pressure — no JSON library needed.
//
// Schema (CLAUDE.md §7.2: user-visible connection status required):
//   wifi_state, rssi, retry_count, last_error, free_heap,
//   rtsp_clients, http_clients, uptime_seconds, reboot_reason, reboot_count
// ---------------------------------------------------------------------------

static String build_status_json()
{
    WiFiStatus wifi = network_get_status();

    const char* state_str;
    switch (wifi.state) {
    case WiFiState::STA_CONNECTED:    state_str = "STA_CONNECTED";    break;
    case WiFiState::STA_CONNECTING:   state_str = "STA_CONNECTING";   break;
    case WiFiState::AP_MODE:          state_str = "AP_MODE";          break;
    case WiFiState::PROVISIONING:     state_str = "PROVISIONING";     break;
    case WiFiState::DISCONNECTED:
    default:                          state_str = "DISCONNECTED";     break;
    }

    // Escape last_error for JSON — replace " with ' and \ with space.
    char escaped_error[72];
    size_t j = 0;
    for (size_t i = 0; wifi.last_error[i] != '\0' && j < sizeof(escaped_error) - 1; i++) {
        char c = wifi.last_error[i];
        if (c == '"')       { escaped_error[j++] = '\''; }
        else if (c == '\\') { escaped_error[j++] = ' ';  }
        else                { escaped_error[j++] = c;     }
    }
    escaped_error[j] = '\0';

    // Build JSON with snprintf into a fixed buffer.
    // STATUS_JSON_BUF_SIZE bytes is sufficient for this payload.
    char json_buf[STATUS_JSON_BUF_SIZE];
    int written = snprintf(json_buf, sizeof(json_buf),
        "{"
          "\"wifi_state\":\"%s\","
          "\"rssi\":%d,"
          "\"retry_count\":%u,"
          "\"last_error\":\"%s\","
          "\"free_heap\":%u,"
          "\"rtsp_clients\":%u,"
          "\"http_clients\":%u,"
          "\"uptime_seconds\":%lu,"
          "\"reboot_reason\":%u,"
          "\"reboot_count\":%lu"
        "}",
        state_str,
        wifi.rssi,
        wifi.retry_count,
        escaped_error,
        ESP.getFreeHeap(),
        health_get_rtsp_client_count(),
        health_get_http_client_count(),
        wifi.uptime_seconds,
        health_get_last_reboot_reason(),
        health_get_reboot_count()
    );

    if (written < 0 || written >= (int)sizeof(json_buf)) {
        log_e("build_status_json: snprintf overflow — %d bytes needed, buffer is %u",
              written, (unsigned)sizeof(json_buf));
        return String("{\"error\":\"JSON buffer overflow\"}");
    }

    return String(json_buf);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t web_ui_init()
{
    if (g_async_server != nullptr) {
        log_w("web_ui_init: already initialized");
        return ESP_OK;
    }

    // --- Heap guard before allocating AsyncWebServer (CLAUDE.md §6) ---
    // The AsyncWebServer object + internal AsyncTCP buffers can exceed 4KB.
    // Reject early if heap is critically low rather than risking a crash.
    if (!heap_can_allocate(ASYNC_WEB_SERVER_ESTIMATED_BYTES)) {
        log_e("web_ui_init: heap guard tripped — insufficient memory for AsyncWebServer");
        return ESP_ERR_NO_MEM;
    }

    // --- Create AsyncWebServer on WEB_UI_PORT ---
    g_async_server = new AsyncWebServer(WEB_UI_PORT);
    if (g_async_server == nullptr) {
        log_e("web_ui_init: failed to allocate AsyncWebServer");
        return ESP_ERR_NO_MEM;
    }

    // --- Register route handlers ---
    // All handlers are async callbacks that run in AsyncTCP's task context,
    // not in task_web_ui (CLAUDE.md §7.1: task_web_ui is supervisory).
    //
    // Handler order: more-specific routes first, catch-all last.

    g_async_server->on("/stream", HTTP_GET, handle_stream);
    g_async_server->on("/snapshot", HTTP_GET, handle_snapshot);
    g_async_server->on("/status", HTTP_GET, handle_status);
    g_async_server->on("/config", HTTP_GET, handle_config);
    g_async_server->on("/", HTTP_GET, handle_root);

    // Captive portal detection — any unknown URL in AP mode is redirected
    // to IotWebConf's config portal. In STA mode, returns 404.
    g_async_server->onNotFound(handle_not_found);

    // --- Start server ---
    g_async_server->begin();

    // --- Build initial caches ---
    g_cached_status_json = build_status_json();
    // Defer template render — moustache_render() does many String allocs/reallocs
    // which can trigger heap corruption on fragmented startup heap (especially
    // after NVS erase + AP mode). Rendered lazily on first GET / request.
    g_cache_valid = false;

    log_i("web_ui_init: AsyncWebServer started on port %d, IotWebConf config on port %d",
          WEB_UI_PORT, IOTWEBCONF_WEB_PORT);
    return ESP_OK;
}

void web_ui_invalidate_cache()
{
    g_cache_valid = false;
    log_i("web_ui_invalidate_cache: template cache invalidated");
}

void web_ui_update_status()
{
    g_cached_status_json = build_status_json();

    // Template cache is NOT invalidated here — status JSON is separate.
    // Template invalidation only happens on settings change or WiFi state
    // change (via web_ui_invalidate_cache() from IotWebConf callback or
    // task_network's state-change detection).
}
