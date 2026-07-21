// task_network.cpp — WiFi lifecycle, retry/backoff state machine, MQTT (future).
//
// CLAUDE.md §5: pinned to core 1, priority 2, 4096-word stack.
// CLAUDE.md §7.2: WiFi retry with exponential backoff, disconnect debounce.
// IotWebConf doLoop() housekeeping runs in non-blocking mode per CLAUDE.md §7.1.
//
// This task owns the WiFi state machine via network_manager. All WiFi/IotWebConf
// logic lives in network_manager; this file is a thin orchestration loop that
// calls network_tick() at ~1 Hz.

#include "task_definitions.h"
#include "network_manager.h"

#include <Arduino.h>
#include <esp_task_wdt.h>

void task_network(void *pvParameters)
{
    esp_err_t init_result = network_init();
    if (init_result != ESP_OK) {
        log_e("task_network: network_init() failed with error 0x%x", init_result);
        // If initialization failed, we can't recover — the WiFi state machine
        // is the core of this task. Delay and let the TWDT handle it (Step 6).
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(TASK_NETWORK_LOOP_MS));
        }
    }

    while (1) {
        WiFiState state = network_tick();

        // Reset TWDT every loop iteration (~1 second).
        // CLAUDE.md §8: every long-running task must reset TWDT within the
        // 10-second timeout window. This task loops every 1 second, well
        // within the budget.
        esp_task_wdt_reset();

        // TODO: Step 7 — signal state changes to web UI via event group
        //       so the status page auto-updates.

        // Log state changes for serial diagnostics.
        // In production (CORE_DEBUG_LEVEL=ERROR), state-change logs are at
        // INFO level and will be compiled out.
        static WiFiState last_state = WiFiState::DISCONNECTED;
        if (state != last_state) {
            log_i("WiFi state changed: %d → %d",
                  static_cast<uint8_t>(last_state),
                  static_cast<uint8_t>(state));
            last_state = state;
        }

        // ~1 Hz tick rate. The state machine is event-driven (via event group
        // bits from the WiFi event handler), so a 1-second polling interval is
        // adequate for connection state changes which happen on the order of
        // seconds, not milliseconds.
        vTaskDelay(pdMS_TO_TICKS(TASK_NETWORK_LOOP_MS));
    }
}
