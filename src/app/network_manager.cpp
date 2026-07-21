// network_manager.cpp — WiFi state machine with retry/backoff and IotWebConf integration.
//
// CLAUDE.md §3: network_manager owns WiFi lifecycle, retry/backoff state machine,
// and IotWebConf integration scoped strictly to provisioning.
//
// CLAUDE.md §7.2 mandates:
//   - Exponential backoff retry: 3 attempts at 1s/2s/4s before AP fallback
//   - 5-second disconnect debounce (WIFI_DISCONNECT_DEBOUNCE_MS)
//   - WiFi.setAutoReconnect(true) at the ESP-IDF WiFi layer
//   - No hardcoded STA credentials — they come from IotWebConf NVS storage only
//
// CLAUDE.md §7.1: IotWebConf is scoped to provisioning only; doLoop() runs here
// in non-blocking mode from task_network.
//
// CLAUDE.md §12: WIFI_SSID / WIFI_PASSWORD in settings.h identify the device's
// own AP (thingName + AP password). These are NOT STA credentials. STA credentials
// are stored exclusively in NVS via IotWebConf provisioning.

#include "network_manager.h"
#include "camera_manager.h"
#include "web_ui_service.h"
#include "config.h"
#include "settings.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_task_wdt.h>

#include <IotWebConf.h>
#include <IotWebConfTParameter.h>
#include <lookup_camera_effect.h>
#include <lookup_camera_frame_size.h>
#include <lookup_camera_gainceiling.h>
#include <lookup_camera_wb_mode.h>

// ===========================================================================
// Internal Constants
// ===========================================================================

// FreeRTOS event group bits — thread-safe signaling from WiFi event context
// (runs in WiFi task) to the state machine (runs in task_network).
// All state mutations happen in network_tick(), never in the event handler.
#define WIFI_EVT_GOT_IP         BIT0
#define WIFI_EVT_DISCONNECTED   BIT1
#define WIFI_EVT_CONFIG_SAVED   BIT2

// ===========================================================================
// File-Scope State — all owned by network_manager, not exposed in header
// ===========================================================================

static WiFiState          g_state               = WiFiState::DISCONNECTED;
static uint8_t            g_retry_count         = 0;
static uint32_t           g_last_disconnect_ms  = 0;      // Timestamp of last debounced disconnect event
static uint32_t           g_state_enter_ms      = 0;      // Timestamp when current state was entered
static uint32_t           g_retry_start_ms      = 0;      // millis() timestamp when current retry backoff started
static bool               g_retry_pending        = false;  // True when a retry backoff is in progress
static uint32_t           g_connected_since_ms  = 0;      // millis() timestamp when STA_CONNECTED was entered; 0 if never connected
static char               g_last_error[64]      = {0};
static bool               g_initialized         = false;
static volatile bool       g_network_ready       = false;   // Set true at end of network_init() — signals task_rtsp that LWIP is ready
static uint32_t           g_frame_duration_ms   = DEFAULT_FRAME_DURATION;  // Cached from IotWebConf for cross-module access

static EventGroupHandle_t g_wifi_event_group    = nullptr;

// ===========================================================================
// IotWebConf Objects — owned by network_manager (CLAUDE.md §7.1)
// ===========================================================================

static DNSServer   g_dns_server;
static WebServer   g_web_server(IOTWEBCONF_WEB_PORT);  // CLAUDE.md §7.1: IotWebConf on separate port from AsyncWebServer

// thingName must live as long as IotWebConf — .c_str() on a temporary String
// is a dangling pointer after the expression ends. Use a static String.
// WIFI_SSID is a DEVICE IDENTITY constant, NOT a hardcoded STA credential.
// STA credentials are stored exclusively in NVS via IotWebConf provisioning.
static String      g_thing_name = String(WIFI_SSID) + "-" + String(ESP.getEfuseMac(), 16);
static IotWebConf  g_iot_web_conf(
    g_thing_name.c_str(),
    &g_dns_server,
    &g_web_server,
    WIFI_PASSWORD,    // AP password (nullptr = open AP). Device identity, not STA credential.
    CONFIG_VERSION
);

// ===========================================================================
// IotWebConf Parameter Groups & Parameter Definitions
//   Moved from main.cpp per CLAUDE.md §3, §7.1.
//   Registered in network_init() with IotWebConf.
//   The config-saved callback routes parameter changes to the appropriate
//   manager (e.g., camera_set_quality() via camera_manager).
// ===========================================================================

static auto param_group_camera = iotwebconf::ParameterGroup("camera", "Camera settings");
static auto param_frame_duration = iotwebconf::Builder<iotwebconf::UIntTParameter<unsigned long>>("fd").label("Frame duration (ms)").defaultValue(DEFAULT_FRAME_DURATION).min(10).build();
static auto param_frame_size = iotwebconf::Builder<iotwebconf::SelectTParameter<sizeof(frame_sizes[0])>>("fs").label("Frame size").optionValues((const char *)&frame_sizes).optionNames((const char *)&frame_sizes).optionCount(sizeof(frame_sizes) / sizeof(frame_sizes[0])).nameLength(sizeof(frame_sizes[0])).defaultValue(DEFAULT_FRAME_SIZE).build();
static auto param_jpg_quality = iotwebconf::Builder<iotwebconf::UIntTParameter<byte>>("q").label("JPG quality").defaultValue(DEFAULT_JPEG_QUALITY).min(1).max(100).build();
static auto param_brightness = iotwebconf::Builder<iotwebconf::IntTParameter<int>>("b").label("Brightness").defaultValue(DEFAULT_BRIGHTNESS).min(-2).max(2).build();
static auto param_contrast = iotwebconf::Builder<iotwebconf::IntTParameter<int>>("c").label("Contrast").defaultValue(DEFAULT_CONTRAST).min(-2).max(2).build();
static auto param_saturation = iotwebconf::Builder<iotwebconf::IntTParameter<int>>("s").label("Saturation").defaultValue(DEFAULT_SATURATION).min(-2).max(2).build();
static auto param_special_effect = iotwebconf::Builder<iotwebconf::SelectTParameter<sizeof(camera_effects[0])>>("e").label("Effect").optionValues((const char *)&camera_effects).optionNames((const char *)&camera_effects).optionCount(sizeof(camera_effects) / sizeof(camera_effects[0])).nameLength(sizeof(camera_effects[0])).defaultValue(DEFAULT_EFFECT).build();
static auto param_whitebal = iotwebconf::Builder<iotwebconf::CheckboxTParameter>("wb").label("White balance").defaultValue(DEFAULT_WHITE_BALANCE).build();
static auto param_awb_gain = iotwebconf::Builder<iotwebconf::CheckboxTParameter>("awbg").label("Automatic white balance gain").defaultValue(DEFAULT_WHITE_BALANCE_GAIN).build();
static auto param_wb_mode = iotwebconf::Builder<iotwebconf::SelectTParameter<sizeof(camera_wb_modes[0])>>("wbm").label("White balance mode").optionValues((const char *)&camera_wb_modes).optionNames((const char *)&camera_wb_modes).optionCount(sizeof(camera_wb_modes) / sizeof(camera_wb_modes[0])).nameLength(sizeof(camera_wb_modes[0])).defaultValue(DEFAULT_WHITE_BALANCE_MODE).build();
static auto param_exposure_ctrl = iotwebconf::Builder<iotwebconf::CheckboxTParameter>("ec").label("Exposure control").defaultValue(DEFAULT_EXPOSURE_CONTROL).build();
static auto param_aec2 = iotwebconf::Builder<iotwebconf::CheckboxTParameter>("aec2").label("Auto exposure (dsp)").defaultValue(DEFAULT_AEC2).build();
static auto param_ae_level = iotwebconf::Builder<iotwebconf::IntTParameter<int>>("ael").label("Auto Exposure level").defaultValue(DEFAULT_AE_LEVEL).min(-2).max(2).build();
static auto param_aec_value = iotwebconf::Builder<iotwebconf::IntTParameter<int>>("aecv").label("Manual exposure value").defaultValue(DEFAULT_AEC_VALUE).min(9).max(1200).build();
static auto param_gain_ctrl = iotwebconf::Builder<iotwebconf::CheckboxTParameter>("gc").label("Gain control").defaultValue(DEFAULT_GAIN_CONTROL).build();
static auto param_agc_gain = iotwebconf::Builder<iotwebconf::IntTParameter<int>>("agcg").label("AGC gain").defaultValue(DEFAULT_AGC_GAIN).min(0).max(30).build();
static auto param_gain_ceiling = iotwebconf::Builder<iotwebconf::SelectTParameter<sizeof(camera_gain_ceilings[0])>>("gcl").label("Auto Gain ceiling").optionValues((const char *)&camera_gain_ceilings).optionNames((const char *)&camera_gain_ceilings).optionCount(sizeof(camera_gain_ceilings) / sizeof(camera_gain_ceilings[0])).nameLength(sizeof(camera_gain_ceilings[0])).defaultValue(DEFAULT_GAIN_CEILING).build();
static auto param_bpc = iotwebconf::Builder<iotwebconf::CheckboxTParameter>("bpc").label("Black pixel correct").defaultValue(DEFAULT_BPC).build();
static auto param_wpc = iotwebconf::Builder<iotwebconf::CheckboxTParameter>("wpc").label("White pixel correct").defaultValue(DEFAULT_WPC).build();
static auto param_raw_gma = iotwebconf::Builder<iotwebconf::CheckboxTParameter>("rg").label("Gamma correct").defaultValue(DEFAULT_RAW_GAMMA).build();
static auto param_lenc = iotwebconf::Builder<iotwebconf::CheckboxTParameter>("lenc").label("Lens correction").defaultValue(DEFAULT_LENC).build();
static auto param_hmirror = iotwebconf::Builder<iotwebconf::CheckboxTParameter>("hm").label("Horizontal mirror").defaultValue(DEFAULT_HORIZONTAL_MIRROR).build();
static auto param_vflip = iotwebconf::Builder<iotwebconf::CheckboxTParameter>("vm").label("Vertical mirror").defaultValue(DEFAULT_VERTICAL_MIRROR).build();
static auto param_dcw = iotwebconf::Builder<iotwebconf::CheckboxTParameter>("dcw").label("Downsize enable").defaultValue(DEFAULT_DCW).build();
static auto param_colorbar = iotwebconf::Builder<iotwebconf::CheckboxTParameter>("cb").label("Colorbar").defaultValue(DEFAULT_COLORBAR).build();

// ===========================================================================
// Forward Declarations (file-scope helpers)
// ===========================================================================

static void wifi_event_handler(WiFiEvent_t event, WiFiEventInfo_t info);
static void on_config_saved();
static iotwebconf::WifiAuthInfo* on_wifi_connection_failed();
static void update_camera_settings();

// ===========================================================================
// WiFi Event Handler
//   Runs in WiFi task context. Uses event group for thread-safe signaling;
//   all state mutations happen in network_tick().
//   Disconnect debounce (CLAUDE.md §7.2): ignore transient disconnects shorter
//   than WIFI_DISCONNECT_DEBOUNCE_MS to prevent reconnect storms.
// ===========================================================================

static void wifi_event_handler(WiFiEvent_t event, WiFiEventInfo_t info)
{
    switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP: {
        // DHCP completed — signal the state machine to transition to STA_CONNECTED.
        // The IP address is available in info.got_ip but we defer logging to
        // network_tick() to keep the ISR path minimal.
        xEventGroupSetBits(g_wifi_event_group, WIFI_EVT_GOT_IP);
        break;
    }

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
        // CLAUDE.md §7.2: debounce disconnect events.
        // WiFi glitches lasting < WIFI_DISCONNECT_DEBOUNCE_MS are ignored.
        // Only continuous disconnect for the full debounce period triggers
        // the reconnect state machine.
        uint32_t now = millis();
        uint32_t elapsed = now - g_last_disconnect_ms;

        if (elapsed >= WIFI_DISCONNECT_DEBOUNCE_MS) {
            g_last_disconnect_ms = now;
            xEventGroupSetBits(g_wifi_event_group, WIFI_EVT_DISCONNECTED);
        }
        // else: transient glitch — suppressed.
        break;
    }

    default:
        break;
    }
}

// ===========================================================================
// IotWebConf Callbacks
// ===========================================================================

// Called by IotWebConf when the user saves configuration via the captive portal.
// Signals the state machine to exit AP mode and attempt STA connection with the
// newly stored credentials.
static void on_config_saved()
{
    log_i("IotWebConf config saved — applying camera settings and signaling state machine");

    // Apply any camera parameter changes immediately.
    update_camera_settings();

    // Invalidate the web UI template cache so the next GET / reflects
    // the new settings (CLAUDE.md §7.1: re-render only on parameter change).
    web_ui_invalidate_cache();

    // Signal the state machine to transition from AP_MODE to DISCONNECTED,
    // which will then attempt STA connection with the new credentials.
    xEventGroupSetBits(g_wifi_event_group, WIFI_EVT_CONFIG_SAVED);
}

// Override IotWebConf's default WiFi connection failure handler.
// Default behavior: return nullptr → immediate AP mode fallback on first
// failure. Our override implements retry with exponential backoff:
//   3 attempts with 1s/2s/4s delays before AP fallback (CLAUDE.md §7.2).
//
// Non-blocking retry: TWDT must reset within 10s (CLAUDE.md §8). This handler
// does NOT call delay(). Instead it sets g_retry_pending and g_retry_start_ms,
// and returns nullptr to let IotWebConf enter AP mode temporarily. The backoff
// timer is checked at the top of network_tick() BEFORE doLoop(); when the
// backoff expires, network_tick() calls forceApMode(false) to transition
// IotWebConf back to Connecting state for the next attempt.
//
// Signature note: IotWebConf v3.2.1 failure handler returns WifiAuthInfo*.
//   - nullptr  → IotWebConf enters AP mode
//   - non-null → IotWebConf retries immediately with those credentials
// We always return nullptr here; our external state machine manages the
// retry cycle via forceApMode(false) after the backoff delay.
static iotwebconf::WifiAuthInfo* on_wifi_connection_failed()
{
    g_retry_count++;

    if (g_retry_count < WIFI_RETRY_COUNT) {
        // Exponential backoff: 1s → 2s → 4s.
        // Uses named constants from config.h per CLAUDE.md §4.
        uint32_t backoff_ms;
        switch (g_retry_count) {
        case 1:  backoff_ms = WIFI_RETRY_DELAY_MS_1; break;  // 1s
        case 2:  backoff_ms = WIFI_RETRY_DELAY_MS_2; break;  // 2s
        default: backoff_ms = WIFI_RETRY_DELAY_MS_3; break;  // 4s
        }

        snprintf(g_last_error, sizeof(g_last_error),
                 "WiFi connect failed — retry %d/%d in %lums",
                 g_retry_count, WIFI_RETRY_COUNT, backoff_ms);
        log_w("%s", g_last_error);

        // Non-blocking: start the backoff timer, let IotWebConf enter AP mode
        // temporarily. network_tick() will retry when the timer expires.
        g_retry_start_ms = millis();
        g_retry_pending = true;

        // Return nullptr → IotWebConf enters AP mode. Our state machine
        // will transition back to Connecting after the backoff delay.
        return nullptr;
    }

    // All retries exhausted — reset state and allow permanent AP mode fallback.
    g_retry_count = 0;
    g_retry_pending = false;

    snprintf(g_last_error, sizeof(g_last_error),
             "WiFi retries exhausted (%d attempts) — entering AP mode",
             WIFI_RETRY_COUNT);
    log_w("%s", g_last_error);

    return nullptr;  // AP mode, for real this time
}

// ===========================================================================
// Camera Settings Update
//   Applies all sensor parameters from IotWebConf to the camera hardware.
//   Moved from main.cpp per CLAUDE.md §3.
//   JPEG quality routes through camera_manager's mapping layer (§1, §12).
//   Other sensor params are set directly until camera_manager is extended.
// ===========================================================================

static void update_camera_settings()
{
    sensor_t* camera = esp_camera_sensor_get();
    if (camera == nullptr) {
        log_e("update_camera_settings: unable to get camera sensor handle");
        return;
    }

    // Cache frame duration for cross-module access (not a sensor register).
    g_frame_duration_ms = param_frame_duration.value();

    // JPEG quality — routes through camera_manager's UI→driver mapping.
    // CLAUDE.md §1, §12: raw 0–63 must never be exposed to end users.
    // camera_set_quality() handles the mapping internally.
    camera_set_quality(param_jpg_quality.value());

    // Direct sensor register access for remaining parameters.
    // TODO: migrate each of these to typed camera_manager accessors when
    // camera_manager is extended (Step 3 follow-up).
    camera->set_brightness(camera, param_brightness.value());
    camera->set_contrast(camera, param_contrast.value());
    camera->set_saturation(camera, param_saturation.value());
    camera->set_special_effect(camera, lookup_camera_effect(param_special_effect.value()));
    camera->set_whitebal(camera, param_whitebal.value());
    camera->set_awb_gain(camera, param_awb_gain.value());
    camera->set_wb_mode(camera, lookup_camera_wb_mode(param_wb_mode.value()));
    camera->set_exposure_ctrl(camera, param_exposure_ctrl.value());
    camera->set_aec2(camera, param_aec2.value());
    camera->set_ae_level(camera, param_ae_level.value());
    camera->set_aec_value(camera, param_aec_value.value());
    camera->set_gain_ctrl(camera, param_gain_ctrl.value());
    camera->set_agc_gain(camera, param_agc_gain.value());
    camera->set_gainceiling(camera, lookup_camera_gainceiling(param_gain_ceiling.value()));
    camera->set_bpc(camera, param_bpc.value());
    camera->set_wpc(camera, param_wpc.value());
    camera->set_raw_gma(camera, param_raw_gma.value());
    camera->set_lenc(camera, param_lenc.value());
    camera->set_hmirror(camera, param_hmirror.value());
    camera->set_vflip(camera, param_vflip.value());
    camera->set_dcw(camera, param_dcw.value());
    camera->set_colorbar(camera, param_colorbar.value());
}

// ===========================================================================
// Network Initialization (Public API)
// ===========================================================================

esp_err_t network_init()
{
    if (g_initialized) {
        log_w("network_init: already initialized");
        return ESP_OK;
    }

    // Create event group before registering any callbacks that might signal it.
    g_wifi_event_group = xEventGroupCreate();
    if (g_wifi_event_group == nullptr) {
        log_e("network_init: failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    // -------------------------------------------------------------------
    // 1. Determine whether STA credentials exist in NVS.
    //    IotWebConf stores credentials via the Preferences library.
    //    If credentials exist, we skip the ~30-second AP mode delay on
    //    cold boot and go directly to STA connection.
    //    CLAUDE.md §7.2: cold boot must not waste time in AP mode when
    //    valid credentials are available.
    // -------------------------------------------------------------------
    bool has_creds = network_has_credentials();
    if (has_creds) {
        g_iot_web_conf.skipApStartup();
        log_i("network_init: credentials found in NVS — skipping AP startup");
    } else {
        log_i("network_init: no credentials found — device is unprovisioned, AP mode will be active");
    }

    // -------------------------------------------------------------------
    // 2. Register IotWebConf callbacks before init().
    //    - Config saved: apply camera settings + signal state machine
    //    - WiFi failed: retry with backoff instead of immediate AP fallback
    // -------------------------------------------------------------------
    g_iot_web_conf.setConfigSavedCallback(on_config_saved);
    g_iot_web_conf.setWifiConnectionFailedHandler(on_wifi_connection_failed);

    // -------------------------------------------------------------------
    // 3. Register IotWebConf parameter groups.
    //    Camera parameters live here because IotWebConf owns the NVS-backed
    //    config storage and web UI form generation. The callbacks route
    //    changes to camera_manager where applicable.
    // -------------------------------------------------------------------
    g_iot_web_conf.addParameterGroup(&param_group_camera);

    param_group_camera.addItem(&param_frame_duration);
    param_group_camera.addItem(&param_frame_size);
    param_group_camera.addItem(&param_jpg_quality);
    param_group_camera.addItem(&param_brightness);
    param_group_camera.addItem(&param_contrast);
    param_group_camera.addItem(&param_saturation);
    param_group_camera.addItem(&param_special_effect);
    param_group_camera.addItem(&param_whitebal);
    param_group_camera.addItem(&param_awb_gain);
    param_group_camera.addItem(&param_wb_mode);
    param_group_camera.addItem(&param_exposure_ctrl);
    param_group_camera.addItem(&param_aec2);
    param_group_camera.addItem(&param_ae_level);
    param_group_camera.addItem(&param_aec_value);
    param_group_camera.addItem(&param_gain_ctrl);
    param_group_camera.addItem(&param_agc_gain);
    param_group_camera.addItem(&param_gain_ceiling);
    param_group_camera.addItem(&param_bpc);
    param_group_camera.addItem(&param_wpc);
    param_group_camera.addItem(&param_raw_gma);
    param_group_camera.addItem(&param_lenc);
    param_group_camera.addItem(&param_hmirror);
    param_group_camera.addItem(&param_vflip);
    param_group_camera.addItem(&param_dcw);
    param_group_camera.addItem(&param_colorbar);

    // -------------------------------------------------------------------
    // 4. Register IotWebConf route handlers on the synchronous WebServer.
    //    Must be BEFORE init() — IotWebConf.init() may internally clear
    //    or reconfigure the WebServer. handleConfig() serves the config
    //    form on GET /, handleNotFound() handles captive portal redirects.
    //    Without these, all requests to port 8080 return "Not found: /".
    // -------------------------------------------------------------------
    g_web_server.on("/", []() { g_iot_web_conf.handleConfig(); });
    g_web_server.onNotFound([]() { g_iot_web_conf.handleNotFound(); });

    // -------------------------------------------------------------------
    // 5. Initialize IotWebConf.
    //    This reads NVS config, sets up the internal state machine, and
    //    prepares the web server. Must be called after all callbacks,
    //    parameters, AND route handlers are registered.
    // -------------------------------------------------------------------
    g_iot_web_conf.init();

    // -------------------------------------------------------------------
    // 5. Force WiFi driver + LWIP TCP/IP stack to initialize NOW.
    //    Must be BEFORE any WiFiServer::begin() calls (WebServer, RTSP).
    //    IotWebConf.init() with skipApStartup() schedules WiFi.begin()
    //    asynchronously via its internal state machine. WiFi.mode(WIFI_STA)
    //    forces the WiFi driver + LWIP TCP/IP task to start synchronously.
    //    Without this, WiFiServer::begin() crashes at tcpip.c:455.
    //    CLAUDE.md §7.2: WiFi.setAutoReconnect(true) is mandatory.
    // -------------------------------------------------------------------
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);

    // -------------------------------------------------------------------
    // 6. Explicitly start the synchronous WebServer on port 8080.
    //    Now safe — LWIP is running (WiFi.mode above). IotWebConf only
    //    calls begin() when entering AP mode. If the device has stored
    //    credentials and goes straight to STA mode (skipApStartup), the
    //    WebServer never starts listening without this explicit call.
    // -------------------------------------------------------------------
    g_web_server.begin();

    // -------------------------------------------------------------------
    // 6. Register WiFi event handler.
    //    Uses SYSTEM_EVENT_* constants for compatibility with ESP32 Arduino
    //    core 2.x. The handler runs in the WiFi task context and only sets
    //    event group bits — all state mutations happen in network_tick().
    // -------------------------------------------------------------------
    WiFi.onEvent(wifi_event_handler, ARDUINO_EVENT_WIFI_STA_GOT_IP);
    WiFi.onEvent(wifi_event_handler, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

    // -------------------------------------------------------------------
    // 7. Set initial state.
    //    - If credentials exist: DISCONNECTED → network_tick() transitions
    //      to STA_CONNECTING on its first cycle.
    //    - If no credentials: AP_MODE — IotWebConf already started the
    //      captive portal via init().
    // -------------------------------------------------------------------
    if (has_creds) {
        g_state = WiFiState::DISCONNECTED;
    } else {
        g_state = WiFiState::AP_MODE;
    }
    g_state_enter_ms = millis();
    g_last_error[0] = '\0';
    g_initialized = true;
    g_network_ready = true;  // Signal task_rtsp that LWIP/WiFi stack is ready for socket ops

    log_i("network_init: done — initial state = %d, has_credentials = %d",
          static_cast<uint8_t>(g_state), has_creds);
    return ESP_OK;
}

// ===========================================================================
// State Machine Tick (Public API)
// ===========================================================================

WiFiState network_tick()
{
    if (!g_initialized) {
        return WiFiState::DISCONNECTED;
    }

    // -------------------------------------------------------------------
    // 1. Non-blocking retry: if a retry backoff is pending, check whether
    //    the backoff delay has elapsed. When it has, call forceApMode(false)
    //    to transition IotWebConf from ApMode back to Connecting so it
    //    re-attempts WiFi.begin() with NVS-stored credentials.
    //
    //    Non-blocking: TWDT must reset within 10s (CLAUDE.md §8).
    //    We cannot use delay() here — the backoff is managed via millis()
    //    comparison, and network_tick() returns quickly every cycle.
    // -------------------------------------------------------------------
    if (g_retry_pending) {
        // Backoff array indexed by (g_retry_count - 1):
        //   retry 1 → 1000ms, retry 2 → 2000ms, retry 3 → 4000ms
        static const uint32_t backoff_delays[WIFI_RETRY_COUNT] = {
            WIFI_RETRY_DELAY_MS_1,
            WIFI_RETRY_DELAY_MS_2,
            WIFI_RETRY_DELAY_MS_3
        };

        uint32_t elapsed = millis() - g_retry_start_ms;
        uint32_t target = backoff_delays[g_retry_count - 1];

        if (elapsed >= target) {
            // Backoff expired — trigger IotWebConf to exit AP mode and
            // transition to Connecting. doLoop() (called below) will then
            // call WiFi.begin() with the stored credentials.
            g_retry_pending = false;
            log_i("Retry backoff expired (%lums) — restarting WiFi connection",
                  target);
            g_iot_web_conf.forceApMode(false);
        }
        // else: backoff not yet elapsed — skip, do nothing this tick.
    }

    // -------------------------------------------------------------------
    // 2. Run IotWebConf housekeeping (non-blocking).
    //    CLAUDE.md §7.1: doLoop() runs exclusively from task_network,
    //    never from any other task. It handles:
    //      - DNS server (captive portal)
    //      - Web server (config pages)
    //      - Internal state transitions
    //      - NVS config persistence
    //
    //    WATCHDOG SAFETY (CLAUDE.md §8): doLoop() drives IotWebConf's
    //    SYNCHRONOUS config WebServer (port 8080) via handleClient(). Serving
    //    the config page to a slow client can block here for up to a couple of
    //    seconds (bounded by HTTP_MAX_DATA_WAIT / HTTP_MAX_SEND_WAIT in the
    //    Arduino WebServer). We feed the task watchdog immediately before and
    //    after so a normal config-portal fetch always gets a fresh 10s window
    //    and can never trip the TWDT — which previously rebooted the device,
    //    and the software reset then left the OV2640 unable to re-initialize
    //    (0x105) until a full power cycle. esp_task_wdt_reset() is a no-op if
    //    this task is not yet TWDT-subscribed (early boot), so it is safe here.
    // -------------------------------------------------------------------
    esp_task_wdt_reset();
    g_iot_web_conf.doLoop();
    esp_task_wdt_reset();

    // -------------------------------------------------------------------
    // 3. Read and clear pending events from the WiFi event handler.
    //    Events are set by wifi_event_handler() running in WiFi task
    //    context. We clear all bits we're about to process atomically.
    // -------------------------------------------------------------------
    EventBits_t bits = xEventGroupClearBits(
        g_wifi_event_group,
        WIFI_EVT_GOT_IP | WIFI_EVT_DISCONNECTED | WIFI_EVT_CONFIG_SAVED);

    // -------------------------------------------------------------------
    // 4. State machine transitions.
    //
    //    Architecture note: IotWebConf OWNS the WiFi.begin(ssid, password)
    //    call — it reads credentials from its own NVS storage and passes
    //    them to the WiFi stack. Our state machine does NOT call
    //    WiFi.begin() or WiFi.mode() directly. Instead:
    //
    //      - IotWebConf's doLoop() (called above) manages the internal
    //        connection flow: BOOT→CONNECTING→WiFi.begin(ssid,pass)→ONLINE.
    //      - Our failure handler (on_wifi_connection_failed) injects retry
    //        with exponential backoff (1s/2s/4s) into IotWebConf's flow.
    //      - This state machine monitors WiFi events and IotWebConf state
    //        to provide a clean WiFiStatus for the web UI / health monitor.
    //
    //    CLAUDE.md §7.2: retry with exponential backoff (1s/2s/4s),
    //    disconnect debounce (5s), WiFi.setAutoReconnect(true).
    // -------------------------------------------------------------------

    switch (g_state) {

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    case WiFiState::DISCONNECTED:
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
        // If credentials exist, transition to STA_CONNECTING.
        // The actual WiFi.begin(ssid, password) is handled by IotWebConf's
        // doLoop() — we just track the logical state. IotWebConf was
        // initialized with skipApStartup() if creds existed, so its
        // internal state machine will drive the connection attempt.
        if (network_has_credentials()) {
            log_i("WiFi credentials found — transitioning to STA_CONNECTING");
            g_state = WiFiState::STA_CONNECTING;
            g_state_enter_ms = millis();
        }
        // else: no credentials — remain DISCONNECTED.
        // IotWebConf will have entered AP mode via its own state machine
        // (boot without credentials → NOT_CONFIGURED → AP_MODE).
        break;

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    case WiFiState::STA_CONNECTING:
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
        // IotWebConf is driving the connection attempt via its doLoop().
        // Our failure handler manages retry/backoff when IotWebConf's
        // connection timeout expires. We monitor for the GOT_IP event
        // to confirm success.
        if (bits & WIFI_EVT_GOT_IP) {
            g_retry_count = 0;
            g_last_error[0] = '\0';
            g_connected_since_ms = millis();
            g_state = WiFiState::STA_CONNECTED;
            g_state_enter_ms = millis();
            log_i("WiFi connected — IP: %s, RSSI: %d dBm",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
            break;
        }

        // A debounced disconnect event while connecting. This can happen
        // if the AP rejects the association quickly (wrong password, MAC
        // filter, etc.). Rather than managing retry here (which would
        // duplicate the failure handler), let IotWebConf's timeout +
        // failure handler drive the retry cycle. We just stay in
        // STA_CONNECTING and let the failure handler do its job.
        //
        // If the disconnect persists through all retries, the failure
        // handler transitions IotWebConf to AP mode, and step 4 below
        // will synchronize our state.
        if (bits & WIFI_EVT_DISCONNECTED) {
            log_w("WiFi disconnect event received while connecting — "
                  "letting IotWebConf failure handler manage retry");
        }
        break;

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    case WiFiState::STA_CONNECTED:
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
        if (bits & WIFI_EVT_DISCONNECTED) {
            // Debounced disconnect while connected (CLAUDE.md §7.2:
            // 5-second filter applied in wifi_event_handler).
            // Reset retry counter — this is a fresh reconnection, not
            // a continuation of a failed sequence.
            g_retry_count = 0;
            snprintf(g_last_error, sizeof(g_last_error),
                     "WiFi disconnected — will reconnect");
            log_w("%s", g_last_error);

            g_state = WiFiState::DISCONNECTED;
            g_state_enter_ms = millis();
            // On next tick, DISCONNECTED will see has_credentials() and
            // transition to STA_CONNECTING. IotWebConf's doLoop() will
            // detect WiFi.status() != WL_CONNECTED and drive the
            // reconnection with its own WiFi.begin(ssid, pass).
        }
        break;

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    case WiFiState::AP_MODE:
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
        // IotWebConf doLoop() handles the captive portal: DNS server,
        // web server, config form. We monitor for config-save events.
        if (bits & WIFI_EVT_CONFIG_SAVED) {
            log_i("WiFi configuration saved — leaving AP mode");
            g_retry_count = 0;
            g_last_error[0] = '\0';
            g_state = WiFiState::DISCONNECTED;
            g_state_enter_ms = millis();
            // On next tick, DISCONNECTED will see the new credentials
            // and transition to STA_CONNECTING.
        }
        break;

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    case WiFiState::PROVISIONING:
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
        // Provisioning is handled by IotWebConf internally. We track it
        // as a separate state for web UI clarity.
        if (bits & WIFI_EVT_CONFIG_SAVED) {
            log_i("Provisioning complete — attempting STA connection");
            g_retry_count = 0;
            g_last_error[0] = '\0';
            g_state = WiFiState::DISCONNECTED;
            g_state_enter_ms = millis();
        }
        break;
    }

    // -------------------------------------------------------------------
    // 5. Synchronize with IotWebConf's internal state.
    //    If IotWebConf is in AP mode (e.g., boot without credentials,
    //    or connection timeout without our override), reflect that in
    //    our external state so the web UI shows the correct status.
    // -------------------------------------------------------------------
    iotwebconf::NetworkState iot_state = g_iot_web_conf.getState();
    if (iot_state == iotwebconf::ApMode
        && g_state != WiFiState::AP_MODE
        && g_state != WiFiState::PROVISIONING) {
        g_state = WiFiState::AP_MODE;
        g_state_enter_ms = millis();
    }

    if (iot_state == iotwebconf::NotConfigured
        && g_state != WiFiState::AP_MODE) {
        g_state = WiFiState::AP_MODE;
        g_state_enter_ms = millis();
    }

    return g_state;
}

// ===========================================================================
// Network Status (Public API)
// ===========================================================================

WiFiStatus network_get_status()
{
    WiFiStatus status = {};
    status.state = g_state;
    status.rssi = (g_state == WiFiState::STA_CONNECTED) ? WiFi.RSSI() : 0;
    status.retry_count = g_retry_count;

    strncpy(status.last_error, g_last_error, sizeof(status.last_error) - 1);
    status.last_error[sizeof(status.last_error) - 1] = '\0';

    if (g_connected_since_ms != 0) {
        status.uptime_seconds = (millis() - g_connected_since_ms) / 1000;
    } else {
        status.uptime_seconds = 0;
    }

    return status;
}

// ===========================================================================
// Credential Check (Public API)
// ===========================================================================

bool network_has_credentials()
{
    // After IotWebConf.init() has been called, the state reflects whether
    // NVS contains a stored WiFi configuration.
    // IOTWEBCONF_STATE_NOT_CONFIGURED → no SSID/password in NVS.
    iotwebconf::NetworkState state = g_iot_web_conf.getState();
    return (state != iotwebconf::NotConfigured);
}

bool network_is_ready()
{
    return g_network_ready;
}

bool network_is_ap_mode()
{
    return (g_state == WiFiState::AP_MODE
            || g_state == WiFiState::PROVISIONING);
}

uint16_t network_get_config_port()
{
    return IOTWEBCONF_WEB_PORT;
}

uint32_t network_get_frame_duration_ms()
{
    return g_frame_duration_ms;
}

framesize_t network_get_frame_size()
{
    return lookup_frame_size(param_frame_size.value());
}
