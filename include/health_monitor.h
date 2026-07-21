// health_monitor.h — TWDT initialization, heap guard, client connection caps,
// and reboot-reason logging.
//
// CLAUDE.md §6: heap guard mandatory before any allocation ≥1KB at runtime.
//   Client connection hard caps: RTSP max 3, HTTP max 5. Both rejections
//   return explicit status — never silently drop the connection.
// CLAUDE.md §8: TWDT is mandatory with 10s timeout, panic-on-timeout enabled.
//   Every long-running task must register and periodically reset.
//   Reboot reason must be logged and persisted to NVS on every boot.
// CLAUDE.md §10.1: pure-logic functions (cap check + counter, heap threshold
//   comparison) are testable on host.

#pragma once

#include <esp_err.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// TWDT & Health Task Init
// ---------------------------------------------------------------------------

// Initialize TWDT (TWDT_TIMEOUT_SEC timeout, panic on timeout).
// Registers all task handles from task_definitions.h.
// Logs reboot reason to NVS (calls log_reboot_reason() internally).
// Must be called once in setup(), AFTER all xTaskCreatePinnedToCore calls
// but BEFORE any task enters its steady-state loop.
esp_err_t health_init();

// Task entry point for health monitoring.
// Infinite loop: monitors heap, logs stats, resets TWDT every 5 seconds.
// CLAUDE.md §5: pinned to core 0, priority 1 (idle-adjacent), 2048-word stack.
void task_health(void *pvParameters);

// ---------------------------------------------------------------------------
// Client Connection Caps — RTSP (CLAUDE.md §6)
// ---------------------------------------------------------------------------

// Try to accept a new RTSP client. Returns true if under MAX_RTSP_CLIENTS.
// Increments internal counter on success. Returns false if cap exceeded.
// Call from RTSP accept path BEFORE creating any per-client objects.
bool rtsp_client_accept();

// Release an RTSP client. Decrements counter safely (floor at 0).
// Call when an RTSP client disconnects or the session is torn down.
void rtsp_client_release();

// ---------------------------------------------------------------------------
// Client Connection Caps — HTTP (CLAUDE.md §6)
// ---------------------------------------------------------------------------

// Try to accept a new HTTP client. Returns true if under MAX_HTTP_CLIENTS.
// Increments internal counter on success. Returns false if cap exceeded.
// Call at the start of each HTTP request handler, before any allocation.
bool http_client_accept();

// Release an HTTP client. Decrements counter safely (floor at 0).
// Call when the HTTP response has been sent and the connection is closing.
void http_client_release();

// ---------------------------------------------------------------------------
// Heap Guard (CLAUDE.md §6)
// ---------------------------------------------------------------------------

// Check if a heap allocation of `size` bytes is safe at runtime.
// Returns true only if:
//   1. free_heap >= size (the allocation itself can fit), AND
//   2. free_heap >= HEAP_GUARD_THRESHOLD of total heap (headroom after alloc).
// Must be called before any runtime allocation ≥1KB.
// Logs the rejection (requested size + current free heap) when false.
bool heap_can_allocate(size_t size);

// ---------------------------------------------------------------------------
// Reboot Reason Logging (CLAUDE.md §8)
// ---------------------------------------------------------------------------

// Read esp_reset_reason(), store reason code and increment boot counter
// to NVS under the "health" namespace. Prints reason to Serial.
// Called once during health_init().
void log_reboot_reason();

// ---------------------------------------------------------------------------
// Status Queries — for web UI and soak-test telemetry
// ---------------------------------------------------------------------------

// Get current RTSP client count (thread-safe snapshot).
uint8_t health_get_rtsp_client_count();

// Get current HTTP client count (thread-safe snapshot).
uint8_t health_get_http_client_count();

// Get the last reboot reason code from NVS.
// Returns 0 if NVS read fails (never written, or NVS corruption).
uint8_t health_get_last_reboot_reason();

// Get the reboot counter from NVS (increments on every boot).
// Returns 0 if NVS read fails.
uint32_t health_get_reboot_count();
