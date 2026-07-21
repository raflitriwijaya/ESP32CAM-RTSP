// web_ui_service.h — Async HTTP server, route handlers, template caching.
//
// CLAUDE.md §7.1: ESPAsyncWebServer + AsyncTCP for all HTTP request handling.
// IotWebConf is scoped strictly to provisioning (captive portal, NVS config).
// moustache_render() output must be cached — re-render only when backing
// parameters change.
//
// CLAUDE.md §6: HTTP client cap (MAX_HTTP_CLIENTS=5) enforced at handler entry
// via http_client_accept() / http_client_release().

#pragma once

#include <esp_err.h>
#include <stdint.h>

// Initialize the async web server and register all route handlers.
// IotWebConf is NOT initialized here — it's managed by network_manager.
// Must be called once from task_web_ui after network_manager is ready.
esp_err_t web_ui_init();

// Invalidate the template cache. Call when any backing parameter changes
// (WiFi state, quality, client count, etc.). Next GET / will re-render.
void web_ui_invalidate_cache();

// Update the cached status JSON. Called periodically by task_web_ui
// or whenever a monitored value changes.
void web_ui_update_status();
