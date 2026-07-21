// task_definitions.h — Single source of truth for ALL FreeRTOS task configuration.
//
// CLAUDE.md §3: all task handles, priorities, and stack sizes in ONE place.
// CLAUDE.md §5: every task's priority and stack size must be declared explicitly
// here — no implicit Arduino loop() task reliance for anything beyond thin
// orchestration.
//
// Stack sizes are in words (multiply by 4 for bytes on ESP32).
// Naming convention: UPPER_SNAKE_CASE per CLAUDE.md §4.

#ifndef TASK_DEFINITIONS_H
#define TASK_DEFINITIONS_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ---------------------------------------------------------------------------
// Task Name Strings
// ---------------------------------------------------------------------------
#define TASK_NAME_NETWORK    "task_network"
#define TASK_NAME_RTSP       "task_rtsp"
#define TASK_NAME_CAMERA     "task_camera"
#define TASK_NAME_WEB_UI     "task_web_ui"
#define TASK_NAME_HEALTH     "task_health"

// ---------------------------------------------------------------------------
// Task Core Affinity
// ---------------------------------------------------------------------------
// CLAUDE.md §5: pin WiFi/network work to core 1, camera/streaming work to core 0.
#define TASK_NETWORK_CORE    1
#define TASK_RTSP_CORE       0
#define TASK_CAMERA_CORE     1
#define TASK_WEB_UI_CORE     0
#define TASK_HEALTH_CORE     0

// ---------------------------------------------------------------------------
// Task Priorities
// ---------------------------------------------------------------------------
// CLAUDE.md §5: camera capture gets the highest application priority on its core.
// A stalled UI must never stall capture.
#define PRIO_NETWORK         2
#define PRIO_RTSP            2
#define PRIO_CAMERA          3
#define PRIO_WEB_UI          1
#define PRIO_HEALTH          1   // idle-adjacent

// ---------------------------------------------------------------------------
// Task Stack Sizes (words, per xTaskCreatePinnedToCore API)
// ---------------------------------------------------------------------------
// CLAUDE.md §5 table: 4096/6144/4096/4096/2048 (bytes in spec; used here as
// words for FreeRTOS stack depth — 4 * words = bytes on ESP32 Xtensa).
#define STACK_NETWORK        4096
#define STACK_RTSP           6144
#define STACK_CAMERA         4096
#define STACK_WEB_UI         4096
#define STACK_HEALTH         2048

// ---------------------------------------------------------------------------
// Task Loop Intervals (milliseconds)
// ---------------------------------------------------------------------------
// Each task's vTaskDelay period in its steady-state loop. These are the
// granularities at which each task polls its respective subsystem.
// CLAUDE.md §5: no blocking calls without explicit vTaskDelay/yield.
//
// TASK_RTSP_LOOP_MS matches RTSP_LOOP_INTERVAL_MS from config.h (40ms).
//   Shorter than RTSP_SERVER_TICK_INTERVAL_MS (200ms) so timer checks are prompt.
// TASK_CAMERA_LOOP_MS matches CAMERA_CAPTURE_INTERVAL_MS from config.h (40ms).
//   Targets ~25fps capture cadence at SVGA resolution.
// ---------------------------------------------------------------------------
#define TASK_NETWORK_LOOP_MS    1000  // WiFi state machine tick rate (~1 Hz)
#define TASK_RTSP_LOOP_MS         40  // RTSP server + frame dispatch (matches RTSP_LOOP_INTERVAL_MS)
#define TASK_CAMERA_LOOP_MS       40  // Frame capture cadence (matches CAMERA_CAPTURE_INTERVAL_MS)
#define TASK_WEB_UI_LOOP_MS     2000  // Status JSON rebuild + cache invalidation check
#define TASK_HEALTH_LOOP_MS     5000  // Heap monitoring + TWDT reset interval

// ---------------------------------------------------------------------------
// TWDT Task Registration
// ---------------------------------------------------------------------------
// Number of application tasks registered with the Task Watchdog Timer.
// Must match the handles[] array in health_monitor.cpp exactly — if a new
// task is added to the system, update both the array and this constant.
#define TWDT_REGISTERED_TASK_COUNT  5   // network, rtsp, camera, web_ui, health

// ---------------------------------------------------------------------------
// Global Task Handles (defined in main.cpp)
// ---------------------------------------------------------------------------
extern TaskHandle_t hTaskNetwork;
extern TaskHandle_t hTaskRtsp;
extern TaskHandle_t hTaskCamera;
extern TaskHandle_t hTaskWebUi;
extern TaskHandle_t hTaskHealth;

// ---------------------------------------------------------------------------
// Task Entry Function Prototypes
// ---------------------------------------------------------------------------
void task_network(void *pvParameters);
void task_rtsp(void *pvParameters);
void task_camera(void *pvParameters);
void task_web_ui(void *pvParameters);
void task_health(void *pvParameters);

#endif // TASK_DEFINITIONS_H
