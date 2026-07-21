// task_web_ui.cpp — Supervisory web UI task.
//
// CLAUDE.md §5: pinned to core 0, priority 1 (idle-adjacent), 4096-word stack.
// CLAUDE.md §7.1: Async callbacks run in AsyncTCP's own task context, NOT here.
// This task initializes the async server and periodically updates cached state
// (status JSON). It does NOT handle HTTP requests directly.
//
// CLAUDE.md §8: resets TWDT within 10-second window (loop runs every 2s).
//
// Template cache invalidation: triggered by web_ui_invalidate_cache() when
// WiFi state changes (detected in task_network) or when IotWebConf settings
// are saved (on_config_saved callback in network_manager).

#include "task_definitions.h"
#include "web_ui_service.h"
#include "network_manager.h"

#include <esp_task_wdt.h>
#include <esp32-hal-log.h>

// Last known WiFi state — used to detect state transitions and invalidate
// the template cache so the UI reflects the new connection status.
static WiFiState g_last_wifi_state = WiFiState::DISCONNECTED;

void task_web_ui(void *pvParameters) {
    // -------------------------------------------------------------------
    // Wait for the network/LWIP stack to be ready before initializing the
    // async web server. AsyncWebServer::begin() → AsyncServer::begin() →
    // tcpip_api_call() requires LWIP TCP/IP to be initialized (done by
    // IotWebConf.init() inside network_init()).
    //
    // Without this wait, the LWIP mbox is invalid and the device panics
    // at lwip/src/api/tcpip.c:497.
    // -------------------------------------------------------------------
    log_i("task_web_ui: waiting for network stack (LWIP) to be ready...");
    while (!network_is_ready()) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    log_i("task_web_ui: network stack ready — initializing web server");

    // Initialize the async web server on port 80 (WEB_UI_PORT).
    // Route handlers run in AsyncTCP task context, not here.
    esp_err_t init_result = web_ui_init();
    if (init_result != ESP_OK) {
        log_e("task_web_ui: web_ui_init() failed with error 0x%x", init_result);
        // Can't serve web UI — sleep and let TWDT handle it.
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(TASK_HEALTH_LOOP_MS));
        }
    }

    while (1) {
        // Update cached status JSON (rebuilt from current state every 2s).
        // This is the ONLY responsibility of this task — async handler
        // callbacks serve the cached data without blocking.
        web_ui_update_status();

        // Detect WiFi state transitions and invalidate the template cache.
        // This ensures GET / serves an up-to-date page when the device
        // connects, disconnects, or enters/exits AP mode.
        WiFiStatus status = network_get_status();
        if (status.state != g_last_wifi_state) {
            log_i("task_web_ui: WiFi state changed %d → %d — invalidating template cache",
                  static_cast<uint8_t>(g_last_wifi_state),
                  static_cast<uint8_t>(status.state));
            web_ui_invalidate_cache();
            g_last_wifi_state = status.state;
        }

        // Reset TWDT every loop iteration (2-second window, well within
        // the 10-second TWDT timeout — CLAUDE.md §8).
        esp_task_wdt_reset();

        vTaskDelay(pdMS_TO_TICKS(TASK_WEB_UI_LOOP_MS));
    }
}
