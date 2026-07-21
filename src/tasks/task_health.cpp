// task_health.cpp — TWDT reset, heap watch, reboot-reason logging.
//
// CLAUDE.md §5: pinned to core 0, priority 1 (idle-adjacent), 2048-word stack.
// CLAUDE.md §8: TWDT is mandatory with 10s timeout, panic-on-timeout enabled.
// CLAUDE.md §6: heap guard — reject ops if free heap < 20% of total heap.
//
// The heavy lifting (client caps, heap guard logic, NVS persistence) lives in
// health_monitor.cpp. This file contains only the FreeRTOS task entry function.

#include "task_definitions.h"
#include "health_monitor.h"
#include "config.h"

#include <Arduino.h>
#include <esp_task_wdt.h>

// ===========================================================================
// task_health — Health monitoring loop (CLAUDE.md §5 task table).
//
// Runs every 5 seconds. Monitors free heap, logs warnings when below
// HEAP_GUARD_THRESHOLD, prints telemetry to Serial, and resets TWDT.
// This task is itself registered with TWDT, so it must call
// esp_task_wdt_reset() at least once per iteration.
//
// CLAUDE.md §10.2 (soak test): metrics (free heap, client count, stack
// high watermarks) are logged every 60 seconds to Serial. The 5-second
// loop allows heap monitoring at finer granularity while batching the
// detailed telemetry output every 12th iteration.
// ===========================================================================

void task_health(void *pvParameters)
{
    log_i("task_health: starting health monitor loop");

    uint32_t iteration = 0;

    while (1) {
        // ---------------------------------------------------------------
        // 1. Check free heap against the guard threshold.
        //    CLAUDE.md §6: PSRAM usage ceiling 85%, DRAM guard at 20%.
        // ---------------------------------------------------------------
        uint32_t free_heap = ESP.getFreeHeap();
        uint32_t total_heap = ESP.getHeapSize();
        uint32_t threshold = (uint32_t)(total_heap * HEAP_GUARD_THRESHOLD);

        if (free_heap < threshold) {
            log_e("task_health: HEAP WARNING — free=%u bytes, total=%u, "
                  "threshold=%u (%.0f%%), RTSP=%d, HTTP=%d",
                  free_heap, total_heap, threshold,
                  HEAP_GUARD_THRESHOLD * 100.0f,
                  health_get_rtsp_client_count(),
                  health_get_http_client_count());
        }

        // ---------------------------------------------------------------
        // 2. Log detailed telemetry every 12 iterations (~60 seconds).
        //    Includes stack high watermarks for each task, which helps
        //    detect tasks that are close to stack overflow in the field.
        // ---------------------------------------------------------------
        if (iteration % TELEMETRY_LOG_INTERVAL == 0) {
            log_i("health: heap=%u/%u (%.1f%% free), RTSP=%d/%d, HTTP=%d/%d",
                  free_heap, total_heap,
                  100.0f * free_heap / total_heap,
                  health_get_rtsp_client_count(), MAX_RTSP_CLIENTS,
                  health_get_http_client_count(), MAX_HTTP_CLIENTS);

            // Stack high watermarks — lower values mean the task is using
            // more of its allocated stack. If any drops below ~500 words,
            // the stack size should be increased in task_definitions.h.
            if (hTaskNetwork != nullptr) {
                UBaseType_t hwm = uxTaskGetStackHighWaterMark(hTaskNetwork);
                log_i("health: stack hwm — network=%u words", (unsigned)hwm);
            }
            if (hTaskRtsp != nullptr) {
                UBaseType_t hwm = uxTaskGetStackHighWaterMark(hTaskRtsp);
                log_i("health: stack hwm — rtsp=%u words", (unsigned)hwm);
            }
            if (hTaskCamera != nullptr) {
                UBaseType_t hwm = uxTaskGetStackHighWaterMark(hTaskCamera);
                log_i("health: stack hwm — camera=%u words", (unsigned)hwm);
            }
            if (hTaskWebUi != nullptr) {
                UBaseType_t hwm = uxTaskGetStackHighWaterMark(hTaskWebUi);
                log_i("health: stack hwm — web_ui=%u words", (unsigned)hwm);
            }
            if (hTaskHealth != nullptr) {
                UBaseType_t hwm = uxTaskGetStackHighWaterMark(hTaskHealth);
                log_i("health: stack hwm — health=%u words", (unsigned)hwm);
            }
        }

        // ---------------------------------------------------------------
        // 3. Reset TWDT for this task.
        //    CLAUDE.md §8: every long-running task must reset TWDT within
        //    the 10-second window. This task runs every 5 seconds, so it
        //    resets with ~50% margin.
        // ---------------------------------------------------------------
        esp_task_wdt_reset();

        iteration++;
        vTaskDelay(pdMS_TO_TICKS(TASK_HEALTH_LOOP_MS));
    }
}
