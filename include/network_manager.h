// network_manager.h — WiFi state machine, retry/backoff, IotWebConf integration.
//
// CLAUDE.md §7.2: WiFi retry with exponential backoff (3 attempts, 1s/2s/4s),
// 5-second disconnect debounce, WiFi.setAutoReconnect(true).
// CLAUDE.md §7.1: IotWebConf is scoped to provisioning only, called from task_network.
//
// No hardcoded STA credentials anywhere in this module (CLAUDE.md §12).
// STA credentials are stored exclusively in NVS via IotWebConf provisioning.
// The WIFI_SSID / WIFI_PASSWORD constants from settings.h identify the device's
// own AP (thingName + AP password) — they are NOT STA credentials.

#pragma once

#include <esp_err.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// WiFi State Enumeration — CLVADE.md §7.2 connection state machine
// ---------------------------------------------------------------------------
enum class WiFiState : uint8_t {
    DISCONNECTED,     // No active connection; will attempt STA if creds exist
    STA_CONNECTING,   // WiFi.begin() called, waiting for DHCP / GOT_IP
    STA_CONNECTED,    // STA mode, IP assigned, operational
    AP_MODE,          // SoftAP active, IotWebConf captive portal running
    PROVISIONING      // IotWebConf config in progress (subset of AP_MODE)
};

// ---------------------------------------------------------------------------
// WiFiStatus — snapshot for web UI and health monitor consumption
// ---------------------------------------------------------------------------
struct WiFiStatus {
    WiFiState state;
    int8_t    rssi;               // dBm, from WiFi.RSSI(); 0 if not connected
    uint8_t   retry_count;        // Number of consecutive failed connection attempts
    char      last_error[64];     // Human-readable last error description
    uint32_t  uptime_seconds;     // Seconds since STA_CONNECTED (or 0 if never connected)
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Initialize WiFi state machine, IotWebConf (non-blocking mode), and parameter
// groups. Must be called once from task_network before entering its loop.
//
// Uses skipApStartup() when stored STA credentials exist to avoid wasting
// ~30 seconds in AP mode on cold boot (CLAUDE.md §7.2).
//
// Returns ESP_OK or an appropriate esp_err_t.
esp_err_t network_init();

// Main state machine tick. Call from task_network loop at ~1 Hz.
// Internally calls iotWebConf.doLoop() for non-blocking provisioning
// housekeeping, then processes pending WiFi events and runs state
// transitions.
//
// Returns current state after processing one cycle.
WiFiState network_tick();

// Get current WiFi status snapshot. Safe to call from any task.
// Returns default-initialized struct (DISCONNECTED) if called before
// network_init().
WiFiStatus network_get_status();

// Check whether STA credentials are stored in NVS (via IotWebConf).
// Returns true if credentials exist — used to decide whether to skip
// AP mode startup and go directly to STA connection.
bool network_has_credentials();

// Check whether network_init() has completed successfully.
// task_rtsp MUST wait for this before constructing the RTSP server
// (which calls WiFiServer::begin() requiring LWIP to be initialized).
// Returns true once WiFi/LWIP stack is ready for socket operations.
bool network_is_ready();

// Check whether the device is currently in AP or PROVISIONING mode.
// Used by web_ui_service to decide whether to redirect to IotWebConf's
// config portal (on a different port) for captive portal handling.
bool network_is_ap_mode();

// Get the port number IotWebConf's synchronous WebServer is running on.
// Used by web_ui_service to construct redirect URLs.
uint16_t network_get_config_port();

// Get the current frame duration in milliseconds from IotWebConf settings.
// Returns DEFAULT_FRAME_DURATION if network not yet initialized.
// Used by web_ui_service for template rendering.
uint32_t network_get_frame_duration_ms();

// Get the current frame size from IotWebConf settings.
// Returns framesize_t enum value.
#include <esp_camera.h>
framesize_t network_get_frame_size();
