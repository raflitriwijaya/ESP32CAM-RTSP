// main.cpp — Orchestration ONLY. No business logic.
//
// CLAUDE.md §3: main.cpp creates tasks, initializes managers, and returns.
// If main.cpp exceeds ~150 lines, logic has leaked into it — refactor.
//
// Transitional note (Step 7/8):
//   - Step 3: camera_manager — DONE (camera_init, quality mapping, frame buffer lifecycle)
//   - Step 4: network_manager — DONE (WiFi state machine, IotWebConf, param groups)
//   - IotWebConf parameter definitions, DNSServer, and update_camera_settings()
//     have been moved to network_manager.cpp per CLAUDE.md §3, §7.1.
//   - Step 7: web_ui_service — DONE (AsyncWebServer on port 80, template caching,
//     IotWebConf config portal on port 8080). Synchronous WebServer is used only
//     as IotWebConf's internal transport — our code never calls it directly.
//   - Camera settings are applied by network_manager when IotWebConf config is loaded
//     (on_config_saved callback and during network_init), not from setup().

#include <Arduino.h>
#include <driver/i2c.h>
#include <nvs_flash.h>
#include <settings.h>
#include "config.h"
#include "camera_manager.h"
#include "network_manager.h"
#include "health_monitor.h"
#include "task_definitions.h"

// ---------------------------------------------------------------------------
// Global Objects
// ---------------------------------------------------------------------------

// camera_init_result retained for setup()'s retry loop diagnostic logging.
esp_err_t camera_init_result;

// ---------------------------------------------------------------------------
// Global Task Handles — defined here, declared extern in task_definitions.h
// ---------------------------------------------------------------------------

TaskHandle_t hTaskNetwork = nullptr;
TaskHandle_t hTaskRtsp = nullptr;
TaskHandle_t hTaskCamera = nullptr;
TaskHandle_t hTaskWebUi = nullptr;
TaskHandle_t hTaskHealth = nullptr;

// ---------------------------------------------------------------------------
// setup() — one-time hardware init, then launch all FreeRTOS tasks.
// CLAUDE.md §3: orchestration ONLY, no business logic.
// ---------------------------------------------------------------------------

void setup()
{
  // Brownout detector MUST remain enabled per CLAUDE.md §8, §12.
  // If brownout resets occur, fix the hardware (add bulk capacitance on the
  // 3.3V rail), never disable this protection in firmware.

#ifdef CAMERA_POWER_GPIO
  pinMode(CAMERA_POWER_GPIO, OUTPUT);
  digitalWrite(CAMERA_POWER_GPIO, CAMERA_POWER_ON_LEVEL);
#endif

#ifdef USER_LED_GPIO
  pinMode(USER_LED_GPIO, OUTPUT);
  digitalWrite(USER_LED_GPIO, !USER_LED_ON_LEVEL);
#endif

  Serial.begin(115200);
  Serial.setDebugOutput(true);

#ifdef ARDUINO_USB_CDC_ON_BOOT
  delay(USB_SETTLE_DELAY_MS); // Wait for USB to connect/settle
#endif

  // ---------------------------------------------------------------------------
  // 1. Log reboot reason (CLAUDE.md §8: mandatory on every boot)
  //    Serial-only quick log here; full NVS persistence is done by
  //    health_init() → log_reboot_reason() called after task creation below.
  // ---------------------------------------------------------------------------
  esp_reset_reason_t reset_reason = esp_reset_reason();
  log_i("Reboot reason: %d (%s)", reset_reason,
        reset_reason == ESP_RST_POWERON  ? "Power-on" :
        reset_reason == ESP_RST_EXT      ? "External pin" :
        reset_reason == ESP_RST_SW       ? "Software reset" :
        reset_reason == ESP_RST_PANIC    ? "Exception/panic" :
        reset_reason == ESP_RST_INT_WDT  ? "Interrupt watchdog" :
        reset_reason == ESP_RST_TASK_WDT ? "Task watchdog" :
        reset_reason == ESP_RST_WDT      ? "Other watchdog" :
        reset_reason == ESP_RST_DEEPSLEEP? "Deep sleep" :
        reset_reason == ESP_RST_BROWNOUT ? "Brownout" :
        reset_reason == ESP_RST_SDIO     ? "SDIO" : "Unknown");

  log_i("Core debug level: %d", CORE_DEBUG_LEVEL);
  log_i("CPU Freq: %d Mhz, %d core(s)", getCpuFrequencyMhz(), ESP.getChipCores());
  log_i("Free heap: %d bytes", ESP.getFreeHeap());
  log_i("SDK version: %s", ESP.getSdkVersion());
  log_i("Board: %s", BOARD_NAME);
  log_i("Starting " APP_TITLE "...");

  // ---------------------------------------------------------------------------
  // 2. PSRAM check (must succeed before camera init — framebuffer lives in PSRAM)
  // ---------------------------------------------------------------------------
  if (CAMERA_CONFIG_FB_LOCATION == CAMERA_FB_IN_PSRAM && !psramInit())
    log_e("Failed to initialize PSRAM");

  // ---------------------------------------------------------------------------
  // 3. Initialize camera with retry (CLAUDE.md §8: explicit detection + recovery)
  //    camera_init() uses compile-time defaults (JPEG_QUALITY_DEFAULT from
  //    config.h). User-configured sensor settings (from IotWebConf NVS) are
  //    applied later by network_manager when task_network calls network_init().
  //    This means the camera starts with safe defaults; user preferences are
  //    applied within the first ~1 second when the network task starts up.
  // ---------------------------------------------------------------------------
  for (auto i = 0; i < CAMERA_INIT_RETRY_COUNT; i++)
  {
    camera_init_result = camera_init();
    if (camera_init_result == ESP_OK)
    {
      log_i("Camera initialized successfully (attempt %d/%d)", i + 1, CAMERA_INIT_RETRY_COUNT);
      break;
    }

    esp_camera_deinit();
    log_e("Failed to initialize camera (attempt %d/%d). Error: 0x%0x",
          i + 1, CAMERA_INIT_RETRY_COUNT, camera_init_result);
    delay(CAMERA_INIT_RETRY_DELAY_MS);
  }

  // ---------------------------------------------------------------------------
  // 4. Create all FreeRTOS tasks (CLAUDE.md §5 task table)
  //    Every task gets its handle, core, priority, and stack from
  //    task_definitions.h — no inline magic numbers.
  //
  //    task_network now owns IotWebConf, WiFi state machine, DNS server,
  //    web server, and camera parameter groups via network_manager (Step 4).
  //    task_web_ui will register HTTP route handlers (Step 5).
  //    task_rtsp drives the RTSP server — consumes frames from g_frame_queue
  //      (filled by task_camera) and broadcasts to clients (TD-7).
  //    task_camera is the sole producer of frame buffers — captures via
  //      camera_fb_get() and posts to g_frame_queue (TD-7).
  //    task_health runs heap watch + TWDT reset (Step 6).
  //    health_init() (called after task creation) initializes TWDT and
  //    persists reboot reason to NVS (Step 6).
  //    ORDERING: task_camera must precede task_rtsp (g_frame_queue dependency).
  // ---------------------------------------------------------------------------

  xTaskCreatePinnedToCore(
      task_network, TASK_NAME_NETWORK,
      STACK_NETWORK, NULL,
      PRIO_NETWORK, &hTaskNetwork,
      TASK_NETWORK_CORE);

  // CLAUDE.md TD-7: task_camera must be created BEFORE task_rtsp so that
  // g_frame_queue exists before task_rtsp tries to read from it.
  // task_camera creates the queue during its initialization.
  xTaskCreatePinnedToCore(
      task_camera, TASK_NAME_CAMERA,
      STACK_CAMERA, NULL,
      PRIO_CAMERA, &hTaskCamera,
      TASK_CAMERA_CORE);

  xTaskCreatePinnedToCore(
      task_rtsp, TASK_NAME_RTSP,
      STACK_RTSP, NULL,
      PRIO_RTSP, &hTaskRtsp,
      TASK_RTSP_CORE);

  xTaskCreatePinnedToCore(
      task_web_ui, TASK_NAME_WEB_UI,
      STACK_WEB_UI, NULL,
      PRIO_WEB_UI, &hTaskWebUi,
      TASK_WEB_UI_CORE);

  xTaskCreatePinnedToCore(
      task_health, TASK_NAME_HEALTH,
      STACK_HEALTH, NULL,
      PRIO_HEALTH, &hTaskHealth,
      TASK_HEALTH_CORE);

  // ---------------------------------------------------------------------------
  // 5. Initialize TWDT + health subsystem (CLAUDE.md §6, §8).
  //    TWDT is initialized AFTER all tasks are created so their handles exist
  //    for registration. health_init() also logs and persists the reboot reason
  //    to NVS. Must be called before any task enters its steady-state loop.
  // ---------------------------------------------------------------------------
  esp_err_t health_err = health_init();
  if (health_err != ESP_OK) {
      log_e("health_init() failed with error 0x%x — TWDT not active!", health_err);
  }

  log_i("All tasks created. setup() complete.");
}

// ---------------------------------------------------------------------------
// loop() — Arduino main loop task MUST die immediately.
// CLAUDE.md §5: no implicit Arduino loop() task reliance for anything beyond
// thin orchestration. All work runs in dedicated FreeRTOS tasks.
// ---------------------------------------------------------------------------

void loop()
{
  vTaskDelete(NULL);
}
