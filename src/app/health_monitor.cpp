// health_monitor.cpp — TWDT init, heap watch, client caps, reboot-reason logging.
//
// CLAUDE.md §6: heap guard mandatory before any allocation ≥1KB at runtime.
//   Client connection hard caps: RTSP max 3, HTTP max 5. Both rejections
//   return explicit status — never silently drop the connection.
// CLAUDE.md §8: TWDT is mandatory with 10s timeout, panic-on-timeout enabled.
//   Every long-running task must register and periodically reset within the
//   TWDT window. Reboot reason must be logged and persisted to NVS on every boot.
// CLAUDE.md §10.1: pure-logic functions (cap check + counter, heap threshold
//   comparison) are testable on host — they do not depend on ESP-IDF/Arduino HAL.

#include "health_monitor.h"
#include "task_definitions.h"
#include "config.h"

#include <Arduino.h>
#include <esp_task_wdt.h>
#include <nvs_flash.h>
#include <nvs.h>

// ===========================================================================
// Internal Constants
// ===========================================================================

// NVS namespace and keys for health telemetry persistence.
// CLAUDE.md §8: reboot reason must survive power cycles so field failures
// are diagnosable without a live serial session.
#define HEALTH_NVS_NAMESPACE      "health"
#define HEALTH_NVS_KEY_REBOOT_CNT "reboot_cnt"
#define HEALTH_NVS_KEY_REASON     "last_reason"

// ===========================================================================
// Client Connection Counters — volatile, protected by critical sections.
//
// CLAUDE.md §6: RTSP max 3, HTTP max 5. Counters are volatile to prevent
// compiler optimizations across critical sections. portENTER_CRITICAL /
// portEXIT_CRITICAL provide ISR-safe mutual exclusion without the overhead
// of a full mutex for these simple increment/decrement operations.
//
// Rationale per CLAUDE.md §4: std::atomic is not used because the Arduino
// framework's libstdc++ configuration on Xtensa may not guarantee lock-free
// atomics for all widths. Critical sections are the safe, portable choice
// for this platform.
// ===========================================================================

static volatile int g_rtsp_client_count = 0;
static volatile int g_http_client_count = 0;

// Critical section mutexes for client counters.
// CLAUDE.md §6: critical sections provide ISR-safe mutual exclusion
// without the overhead of a full FreeRTOS mutex.
// ESP32 FreeRTOS port requires a portMUX_TYPE argument.
static portMUX_TYPE g_rtsp_count_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE g_http_count_mux = portMUX_INITIALIZER_UNLOCKED;

// ===========================================================================
// Health Init — TWDT initialization, task registration, reboot-reason logging.
//
// TWDT API note: the older Arduino-esp32 core uses
//   esp_task_wdt_init(uint32_t timeout_seconds, bool panic)
// The newer ESP-IDF 5.x API uses esp_task_wdt_config_t with .timeout_ms.
// This implementation uses the Arduino-compatible API. When migrating to
// ESP-IDF (CLAUDE.md §11), switch to the config-struct API.
// ===========================================================================

esp_err_t health_init()
{
    // -------------------------------------------------------------------
    // 1. Initialize TWDT with panic-on-timeout.
    //    CLAUDE.md §8: 10-second timeout, panic enabled, no soft-recovery.
    //    A task that cannot reset TWDT within its budget is a bug to fix,
    //    not a reason to disable TWDT.
    // -------------------------------------------------------------------
    esp_err_t err = esp_task_wdt_init(TWDT_TIMEOUT_SEC, true);
    if (err != ESP_OK) {
        log_e("health_init: esp_task_wdt_init failed with error 0x%x", err);
        return err;
    }
    log_i("health_init: TWDT initialized — timeout=%ds, panic=enabled",
          TWDT_TIMEOUT_SEC);

    // -------------------------------------------------------------------
    // 2. Register all task handles with TWDT.
    //    CLAUDE.md §8: every long-running task must register and reset
    //    periodically. If a handle is NULL, the task hasn't been created
    //    yet — log a warning but don't crash. The task can be registered
    //    later, or the NULL check means it was never created.
    //
    //    Registration order doesn't matter; all handles get the same
    //    timeout. The TWDT fires if ANY registered task fails to reset
    //    within the timeout window.
    // -------------------------------------------------------------------
    TaskHandle_t handles[] = {
        hTaskNetwork,
        hTaskRtsp,
        hTaskCamera,
        hTaskWebUi,
        hTaskHealth
    };
    const char* names[] = {
        TASK_NAME_NETWORK,
        TASK_NAME_RTSP,
        TASK_NAME_CAMERA,
        TASK_NAME_WEB_UI,
        TASK_NAME_HEALTH
    };

    for (int i = 0; i < TWDT_REGISTERED_TASK_COUNT; i++) {
        if (handles[i] != nullptr) {
            err = esp_task_wdt_add(handles[i]);
            if (err != ESP_OK) {
                log_e("health_init: TWDT add failed for %s — error 0x%x",
                      names[i], err);
                // Don't abort — one task failing to register shouldn't
                // prevent other tasks from being registered. The TWDT
                // will still fire if any registered task hangs.
            } else {
                log_i("health_init: TWDT registered — %s", names[i]);
            }
        } else {
            log_w("health_init: %s handle is NULL — task not created yet, "
                  "skipping TWDT registration", names[i]);
        }
    }

    // The main Arduino task (setup/loop) is intentionally NOT registered
    // with TWDT. loop() calls vTaskDelete(NULL) to kill itself, and a TWDT-
    // subscribed task that deletes itself causes a stale-handle watchdog
    // fire after 10s. All application logic runs in dedicated FreeRTOS
    // tasks (registered above) — the main task is just a launcher.

    // -------------------------------------------------------------------
    // 3. Log and persist reboot reason.
    //    CLAUDE.md §8: reboot reason must be logged and persisted to NVS
    //    on every boot so field failures are diagnosable without a live
    //    serial session.
    // -------------------------------------------------------------------
    log_reboot_reason();

    log_i("health_init: complete — %d RTSP clients, %d HTTP clients",
          g_rtsp_client_count, g_http_client_count);
    return ESP_OK;
}

// ===========================================================================
// Client Connection Caps — RTSP
//
// CLAUDE.md §6: max 3 concurrent RTSP clients. Reject with explicit status
// (503 equivalent for RTSP), never silently drop.
//
// Thread safety: portENTER_CRITICAL / portEXIT_CRITICAL provide ISR-safe
// mutual exclusion. These functions are called from the RTSP server's timer
// callback (rtsp_server::client_handler), not from a FreeRTOS task context,
// so a FreeRTOS mutex would be inappropriate here. Critical sections work
// correctly regardless of calling context on single-core sections of ESP32.
// ===========================================================================

bool rtsp_client_accept()
{
    portENTER_CRITICAL(&g_rtsp_count_mux);
    bool accepted = false;
    if (g_rtsp_client_count < MAX_RTSP_CLIENTS) {
        g_rtsp_client_count++;
        accepted = true;
    }
    portEXIT_CRITICAL(&g_rtsp_count_mux);

    if (!accepted) {
        log_w("rtsp_client_accept: REJECTED — count=%d, cap=%d",
              g_rtsp_client_count, MAX_RTSP_CLIENTS);
    }
    return accepted;
}

void rtsp_client_release()
{
    portENTER_CRITICAL(&g_rtsp_count_mux);
    if (g_rtsp_client_count > 0) {
        g_rtsp_client_count--;
    }
    portEXIT_CRITICAL(&g_rtsp_count_mux);
}

// ===========================================================================
// Client Connection Caps — HTTP
//
// CLAUDE.md §6: max 5 concurrent HTTP/web UI clients. Reject with 503 +
// Retry-After header, never silently drop.
//
// Thread safety: same critical-section approach as RTSP. HTTP handlers
// run in AsyncTCP task context (ESPAsyncWebServer per CLAUDE.md §7.1).
// Critical sections are safe regardless of calling context.
// ===========================================================================

bool http_client_accept()
{
    portENTER_CRITICAL(&g_http_count_mux);
    bool accepted = false;
    if (g_http_client_count < MAX_HTTP_CLIENTS) {
        g_http_client_count++;
        accepted = true;
    }
    portEXIT_CRITICAL(&g_http_count_mux);

    if (!accepted) {
        log_w("http_client_accept: REJECTED — count=%d, cap=%d",
              g_http_client_count, MAX_HTTP_CLIENTS);
    }
    return accepted;
}

void http_client_release()
{
    portENTER_CRITICAL(&g_http_count_mux);
    if (g_http_client_count > 0) {
        g_http_client_count--;
    }
    portEXIT_CRITICAL(&g_http_count_mux);
}

// ===========================================================================
// Heap Guard — pre-allocation safety check.
//
// CLAUDE.md §6: mandatory before any runtime allocation ≥1KB.
// Returns true only if both:
//   1. free_heap >= size — the allocation itself is expected to fit
//   2. free_heap >= HEAP_GUARD_THRESHOLD * total_heap — headroom after alloc
//
// This function is pure logic (arithmetic comparison on ESP HAL getters) and
// is designed to be testable on host per CLAUDE.md §10.1. The ESP.getFreeHeap()
// and ESP.getHeapSize() calls are the only HAL dependency; the logic itself
// is a simple threshold comparison.
// ===========================================================================

bool heap_can_allocate(size_t size)
{
    uint32_t free_heap = ESP.getFreeHeap();
    uint32_t total_heap = ESP.getHeapSize();
    uint32_t threshold = (uint32_t)(total_heap * HEAP_GUARD_THRESHOLD);

    bool can_alloc = (free_heap >= size) && (free_heap >= threshold);

    if (!can_alloc) {
        log_e("heap_can_allocate: REJECTED — requested=%u, free=%u, "
              "total=%u, threshold=%u (%.0f%%)",
              (unsigned)size, free_heap, total_heap, threshold,
              HEAP_GUARD_THRESHOLD * 100.0f);
    }

    return can_alloc;
}

// ===========================================================================
// Reboot Reason Logging — persist to NVS, print to Serial.
//
// CLAUDE.md §8: reboot reason must be logged and persisted on every boot.
// An incrementing reboot counter helps detect crash-looping in the field.
//
// NVS keys used (namespace "health"):
//   "reboot_cnt"  — uint32_t, incremented on every boot
//   "last_reason" — uint8_t, esp_reset_reason() code from most recent boot
//
// If the NVS partition is full or corrupted, the write is skipped with a
// warning — the device continues operating, but field diagnostics are
// degraded until NVS is cleared.
// ===========================================================================

void log_reboot_reason()
{
    esp_reset_reason_t reason = esp_reset_reason();

    // Build human-readable reason string for Serial output.
    // CLAUDE.md §8: serial-monitor-only diagnostics are not sufficient for
    // field-deployed devices — the NVS persistence below is the durable record.
    const char* reason_str;
    switch (reason) {
    case ESP_RST_POWERON:   reason_str = "Power-on";           break;
    case ESP_RST_EXT:        reason_str = "External pin";       break;
    case ESP_RST_SW:         reason_str = "Software reset";     break;
    case ESP_RST_PANIC:      reason_str = "Exception/panic";    break;
    case ESP_RST_INT_WDT:    reason_str = "Interrupt watchdog"; break;
    case ESP_RST_TASK_WDT:   reason_str = "Task watchdog";      break;
    case ESP_RST_WDT:        reason_str = "Other watchdog";     break;
    case ESP_RST_DEEPSLEEP:  reason_str = "Deep sleep";         break;
    case ESP_RST_BROWNOUT:   reason_str = "Brownout";           break;
    case ESP_RST_SDIO:       reason_str = "SDIO";               break;
    default:                 reason_str = "Unknown";            break;
    }

    // -------------------------------------------------------------------
    // NVS persistence.
    // -------------------------------------------------------------------
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(HEALTH_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        log_e("log_reboot_reason: nvs_open failed — error 0x%x", err);
        // Still print to Serial even if NVS is unavailable.
        log_i("Reboot reason: %d (%s) — NVS unavailable, count unknown",
              reason, reason_str);
        return;
    }

    // Read current reboot counter, increment, write back.
    uint32_t reboot_count = 0;
    err = nvs_get_u32(nvs_handle, HEALTH_NVS_KEY_REBOOT_CNT, &reboot_count);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // First boot — counter starts at 1.
        reboot_count = 1;
    } else if (err != ESP_OK) {
        log_w("log_reboot_reason: nvs_get_u32(reboot_cnt) error 0x%x — "
              "resetting to 1", err);
        reboot_count = 1;
    } else {
        reboot_count++;
    }

    err = nvs_set_u32(nvs_handle, HEALTH_NVS_KEY_REBOOT_CNT, reboot_count);
    if (err != ESP_OK) {
        log_e("log_reboot_reason: nvs_set_u32(reboot_cnt) failed — error 0x%x",
              err);
    }

    // Store the reset reason code.
    err = nvs_set_u8(nvs_handle, HEALTH_NVS_KEY_REASON, (uint8_t)reason);
    if (err != ESP_OK) {
        log_e("log_reboot_reason: nvs_set_u8(last_reason) failed — error 0x%x",
              err);
    }

    // Commit and close.
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        log_e("log_reboot_reason: nvs_commit failed — error 0x%x", err);
    }
    nvs_close(nvs_handle);

    // Serial output — visible in the monitor for immediate diagnostics.
    log_i("Reboot reason: %d (%s), count: %u", reason, reason_str, reboot_count);
}

// ===========================================================================
// Status Query Functions — thread-safe snapshots for web UI and soak test.
//
// CLAUDE.md §7.2: user-visible connection status is required; serial-monitor-
// only diagnostics are not acceptable for a field-deployed device.
// These getters provide health telemetry for the web UI status page.
// ===========================================================================

uint8_t health_get_rtsp_client_count()
{
    portENTER_CRITICAL(&g_rtsp_count_mux);
    uint8_t count = (uint8_t)g_rtsp_client_count;
    portEXIT_CRITICAL(&g_rtsp_count_mux);
    return count;
}

uint8_t health_get_http_client_count()
{
    portENTER_CRITICAL(&g_http_count_mux);
    uint8_t count = (uint8_t)g_http_client_count;
    portEXIT_CRITICAL(&g_http_count_mux);
    return count;
}

uint8_t health_get_last_reboot_reason()
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(HEALTH_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return 0;  // NVS unavailable — return 0 (not a valid esp_reset_reason_t)
    }

    uint8_t reason = 0;
    err = nvs_get_u8(nvs_handle, HEALTH_NVS_KEY_REASON, &reason);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        log_w("health_get_last_reboot_reason: nvs_get_u8 error 0x%x", err);
    }
    nvs_close(nvs_handle);
    return reason;
}

uint32_t health_get_reboot_count()
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(HEALTH_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return 0;
    }

    uint32_t count = 0;
    err = nvs_get_u32(nvs_handle, HEALTH_NVS_KEY_REBOOT_CNT, &count);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        log_w("health_get_reboot_count: nvs_get_u32 error 0x%x", err);
    }
    nvs_close(nvs_handle);
    return count;
}
