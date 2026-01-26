#include "wifi_sta/wifi_sta.hpp"

#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

// No exceptions / no RTTI enforced by CMake compile options.

namespace wifi_sta {

static const char* kTag = "wifi_sta";

// -----------------------------
// Backend: netif/event loop
// -----------------------------
esp_err_t EspIdfWifiBackend::init_event_loop_once() {
  esp_err_t err = esp_event_loop_create_default();
  if (err == ESP_ERR_INVALID_STATE) {
    // already created -> ok (idempotent)
    return ESP_OK;
  }
  return err;
}

esp_err_t EspIdfWifiBackend::init_netif_once() {
  // In some ESP-IDF versions, calling twice may return ESP_ERR_INVALID_STATE.
  // We accept that as OK per requirement.
  esp_err_t err = esp_netif_init();
  if (err == ESP_ERR_INVALID_STATE) {
    return ESP_OK;
  }
  return err;
}

esp_netif_t* EspIdfWifiBackend::ensure_sta_netif(esp_netif_t* injected_or_null) {
  if (injected_or_null != nullptr) {
    return injected_or_null;
  }

  // Try to reuse default STA netif if it exists.
  esp_netif_t* existing = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (existing != nullptr) {
    return existing;
  }

  // Create default STA netif (must not create duplicates).
  return esp_netif_create_default_wifi_sta();
}

// -----------------------------
// Backend: wifi driver
// -----------------------------
esp_err_t EspIdfWifiBackend::init_wifi() {
  // The wifi init config uses official ESP-IDF macro.
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_err_t err = esp_wifi_init(&cfg);
  if (err != ESP_OK) return err;

  err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err != ESP_OK) return err;

  return ESP_OK;
}

static bool validate_cstr_len_leq(const char* s, size_t max_len_including_null) {
  if (s == nullptr) return false;
  // max_len_including_null includes '\0', so max content len is (max-1)
  const size_t max_content = (max_len_including_null == 0) ? 0 : (max_len_including_null - 1);
  const size_t n = strnlen(s, max_len_including_null);
  // If n == max_len_including_null => no null terminator within allowed buffer => too long
  if (n > max_content) return false;
  return true;
}

esp_err_t EspIdfWifiBackend::set_sta_config_from_ssid_pass(const char* ssid, const char* pass) {
  wifi_config_t cfg;
  std::memset(&cfg, 0, sizeof(cfg));

  // Validate lengths BEFORE writing into wifi_config_t fields.
  if (!validate_cstr_len_leq(ssid, sizeof(cfg.sta.ssid))) return ESP_ERR_INVALID_ARG;
  if (!validate_cstr_len_leq(pass, sizeof(cfg.sta.password))) return ESP_ERR_INVALID_ARG;

  std::memcpy(cfg.sta.ssid, ssid, std::strlen(ssid));
  std::memcpy(cfg.sta.password, pass, std::strlen(pass));

  // Disable internal retry logic (we own reconnection via RetryPolicy + timer).
  cfg.sta.failure_retry_cnt = 0;

  // esp_wifi_set_config requires a mutable pointer.
  return esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

esp_err_t EspIdfWifiBackend::start() { return esp_wifi_start(); }
esp_err_t EspIdfWifiBackend::connect() { return esp_wifi_connect(); }
esp_err_t EspIdfWifiBackend::disconnect() { return esp_wifi_disconnect(); }
esp_err_t EspIdfWifiBackend::stop() { return esp_wifi_stop(); }

esp_err_t EspIdfWifiBackend::deinit_wifi() {
  // Best-effort teardown:
  // - esp_wifi_deinit() is safe after stop (though errors may happen if not init).
  return esp_wifi_deinit();
}

// -----------------------------
// Backend: event handlers
// -----------------------------
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
  (void)event_base;

  HandlerCtx* const ctx = static_cast<HandlerCtx*>(arg);
  if (ctx == nullptr || ctx->self == nullptr) return;

  if (event_id == WIFI_EVENT_STA_START) {
    if (ctx->on_sta_start != nullptr) ctx->on_sta_start(ctx->self);
    return;
  }

  if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
    const wifi_event_sta_disconnected_t* const ev = static_cast<const wifi_event_sta_disconnected_t*>(event_data);
    const int reason = (ev != nullptr) ? static_cast<int>(ev->reason) : -1;
    if (ctx->on_sta_disconnected != nullptr) ctx->on_sta_disconnected(ctx->self, reason);
    return;
  }
}

static void ip_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
  (void)event_base;

  HandlerCtx* const ctx = static_cast<HandlerCtx*>(arg);
  if (ctx == nullptr || ctx->self == nullptr) return;

  if (event_id == IP_EVENT_STA_GOT_IP) {
    const ip_event_got_ip_t* const ev = static_cast<const ip_event_got_ip_t*>(event_data);
    const esp_netif_ip_info_t* ip = (ev != nullptr) ? &ev->ip_info : nullptr;
    if (ctx->on_got_ip != nullptr) ctx->on_got_ip(ctx->self, ip);
    return;
  }

  if (event_id == IP_EVENT_STA_LOST_IP) {
    if (ctx->on_lost_ip != nullptr) ctx->on_lost_ip(ctx->self);
    return;
  }
}

esp_err_t EspIdfWifiBackend::register_handlers(void* ctx, handler_handle_t* out_wifi_h, handler_handle_t* out_ip_h) {
  if (out_wifi_h == nullptr || out_ip_h == nullptr) return ESP_ERR_INVALID_ARG;
  *out_wifi_h = nullptr;
  *out_ip_h = nullptr;

  esp_event_handler_instance_t wifi_inst = nullptr;
  esp_event_handler_instance_t ip_inst = nullptr;

  esp_err_t err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, ctx, &wifi_inst);
  if (err != ESP_OK) return err;

  err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler, ctx, &ip_inst);
  if (err != ESP_OK) {
    (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_inst);
    return err;
  }

  *out_wifi_h = reinterpret_cast<void*>(wifi_inst);
  *out_ip_h = reinterpret_cast<void*>(ip_inst);
  return ESP_OK;
}

esp_err_t EspIdfWifiBackend::unregister_handlers(handler_handle_t wifi_h, handler_handle_t ip_h) {
  esp_err_t err_wifi = ESP_OK;
  esp_err_t err_ip = ESP_OK;

  if (wifi_h != nullptr) {
    err_wifi = esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                     reinterpret_cast<esp_event_handler_instance_t>(wifi_h));
  }
  if (ip_h != nullptr) {
    err_ip = esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID,
                                                   reinterpret_cast<esp_event_handler_instance_t>(ip_h));
  }

  // Return first error if any.
  if (err_wifi != ESP_OK) return err_wifi;
  if (err_ip != ESP_OK) return err_ip;
  return ESP_OK;
}

}  // namespace wifi_sta
