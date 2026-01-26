#pragma once

// Public header intentionally kept "light" to avoid C/C++ ESP-IDF header conflicts.
// Allowed includes (per requirements):
#include <cstdint>

#include "esp_err.h"
#include "esp_netif_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

namespace wifi_sta {

// -----------------------------
// Light, POD-only handler context
// -----------------------------
// This struct is the ONLY thing that crosses the .hpp/.cpp boundary for event callbacks.
// wifi_sta.cpp parses ESP-IDF event payloads (heavy types) and then calls these function pointers.
//
// No virtual, no heap, no RTTI, no exceptions.
struct HandlerCtx {
  void* self;
  void (*on_sta_start)(void* self) noexcept;
  void (*on_sta_disconnected)(void* self, int reason) noexcept;
  void (*on_got_ip)(void* self, const esp_netif_ip_info_t* ip) noexcept;
  void (*on_lost_ip)(void* self) noexcept;
};

// -----------------------------
// Notify policy: EventGroup based (no heap, trivial ops)
// -----------------------------
// Default implementation: sets/clears bits in an EventGroupHandle_t provided via bind().
class EventGroupNotifier {
 public:
  EventGroupNotifier() = default;

  void bind(EventGroupHandle_t eg, EventBits_t connected_bit, EventBits_t has_ip_bit) noexcept {
    eg_ = eg;
    connected_bit_ = connected_bit;
    has_ip_bit_ = has_ip_bit;
  }

  void on_connected() noexcept {
    if (eg_ != nullptr) {
      (void)xEventGroupSetBits(eg_, connected_bit_);
    }
  }

  void on_disconnected(int /*reason*/) noexcept {
    if (eg_ != nullptr) {
      (void)xEventGroupClearBits(eg_, connected_bit_);
      (void)xEventGroupClearBits(eg_, has_ip_bit_);
    }
  }

  void on_got_ip(const esp_netif_ip_info_t& /*ip*/) noexcept {
    if (eg_ != nullptr) {
      (void)xEventGroupSetBits(eg_, has_ip_bit_);
      (void)xEventGroupSetBits(eg_, connected_bit_);
    }
  }

  void on_lost_ip() noexcept {
    if (eg_ != nullptr) {
      (void)xEventGroupClearBits(eg_, has_ip_bit_);
    }
  }

 private:
  EventGroupHandle_t eg_ = nullptr;
  EventBits_t connected_bit_ = 0;
  EventBits_t has_ip_bit_ = 0;
};

// -----------------------------
// Credential policy (v0.1): StaticCredentials
// -----------------------------
struct StaticCredentials {
  static const char* ssid() noexcept { return ""; }
  static const char* password() noexcept { return ""; }
};

// -----------------------------
// Retry policy (v0.1): Default classification + exponential backoff
// -----------------------------
// Note: wifi_reason is an integer reason code from WIFI_EVENT_STA_DISCONNECTED.
// The esp-idf docs provide a reason-code table for ESP32; we keep numbers here to avoid heavy headers.
// You can replace this policy with your own, using esp_wifi_types.h in your .cpp if desired.
//
// Permanent (needs manual intervention): auth/security mismatch / repeated auth failures, etc.
struct DefaultRetryPolicy {
  static bool is_permanent(int wifi_reason) noexcept {
    // Espressif-specific (200+) reasons (common in ESP-IDF Wi-Fi reason table):
    // 202 AUTH_FAIL, 203 ASSOC_FAIL, 204 HANDSHAKE_TIMEOUT,
    // 210 NO_AP_FOUND_SECURITY, 211 NO_AP_FOUND_AUTHMODE, 212 NO_AP_FOUND_RSSI
    if (wifi_reason == 202 || wifi_reason == 203 || wifi_reason == 204) return true;
    if (wifi_reason == 210 || wifi_reason == 211 || wifi_reason == 212) return true;

    // IEEE-ish "invalid" / "security" style reasons are usually permanent, but we keep v0.1 conservative.
    // Treat others as transient.
    return false;
  }

  static bool should_retry(int wifi_reason, int attempt) noexcept {
    if (is_permanent(wifi_reason)) return false;
    // v0.1: allow up to 8 attempts
    return attempt < 8;
  }

  static TickType_t backoff_ticks(int attempt) noexcept {
    // Exponential backoff: 500ms, 1s, 2s, 4s, ... clamp to 30s
    // No jitter (deterministic v0.1).
    const uint32_t base_ms = 500U;
    const uint32_t max_ms = 30000U;

    uint32_t factor = 1U;
    // attempt starts at 1 for first retry
    if (attempt > 0) {
      const int shift = (attempt > 10) ? 10 : attempt; // clamp shift to avoid overflow
      factor = (1U << static_cast<uint32_t>(shift));
    }

    uint32_t ms = base_ms * factor;
    if (ms > max_ms) ms = max_ms;

    return pdMS_TO_TICKS(ms);
  }
};

// -----------------------------
// Backend contract (implemented in wifi_sta.cpp)
// -----------------------------
struct EspIdfWifiBackend {
  // netif/event loop
  static esp_err_t init_event_loop_once();
  static esp_err_t init_netif_once();
  static esp_netif_t* ensure_sta_netif(esp_netif_t* injected_or_null);

  // wifi driver
  static esp_err_t init_wifi();
  static esp_err_t set_sta_config_from_ssid_pass(const char* ssid, const char* pass);
  static esp_err_t start();
  static esp_err_t connect();
  static esp_err_t disconnect();
  static esp_err_t stop();
  static esp_err_t deinit_wifi();

  // handlers (opaque in header)
  using handler_handle_t = void*;
  static esp_err_t register_handlers(void* ctx, handler_handle_t* out_wifi_h, handler_handle_t* out_ip_h);
  static esp_err_t unregister_handlers(handler_handle_t wifi_h, handler_handle_t ip_h);
};

// -----------------------------
// WifiStaManager (template)
// -----------------------------
template <class Backend, class CredentialStore, class RetryPolicy, class Notifier>
class WifiStaManager {
 public:
  WifiStaManager() = default;

  esp_err_t begin(esp_netif_t* injected_sta_netif = nullptr) noexcept {
    // Create static RTOS primitives (once)
    ensure_event_group_created_();
    ensure_timer_created_();

    // Reset state for a fresh begin()
    clear_all_state_();

    // Bind notifier to our EventGroup bits (optional, default notifier supports it)
    notifier_.bind(event_group_, kBitConnected, kBitHasIp);

    // Prepare handler context that lives inside this object (lifetime-safe until end())
    handler_ctx_.self = this;
    handler_ctx_.on_sta_start = &WifiStaManager::ctx_on_sta_start_;
    handler_ctx_.on_sta_disconnected = &WifiStaManager::ctx_on_sta_disconnected_;
    handler_ctx_.on_got_ip = &WifiStaManager::ctx_on_got_ip_;
    handler_ctx_.on_lost_ip = &WifiStaManager::ctx_on_lost_ip_;

    // Begin lifecycle (deterministic)
    esp_err_t err = Backend::init_event_loop_once();
    if (err != ESP_OK) return err;

    err = Backend::init_netif_once();
    if (err != ESP_OK) return err;

    sta_netif_ = Backend::ensure_sta_netif(injected_sta_netif);
    if (sta_netif_ == nullptr) return ESP_FAIL;

    err = Backend::init_wifi();
    if (err != ESP_OK) {
      rollback_();
      return err;
    }
    wifi_inited_ = true;

    err = Backend::set_sta_config_from_ssid_pass(CredentialStore::ssid(), CredentialStore::password());
    if (err != ESP_OK) {
      rollback_();
      return err;
    }

    // Register handlers BEFORE start/connect to receive events consistently.
    begun_ = true;
    err = Backend::register_handlers(static_cast<void*>(&handler_ctx_), &wifi_handler_, &ip_handler_);
    if (err != ESP_OK) {
      begun_ = false;
      rollback_();
      return err;
    }
    handlers_registered_ = true;

    err = Backend::start();
    if (err != ESP_OK) {
      rollback_();
      return err;
    }
    wifi_started_ = true;

    // Connect outside event handlers (allowed, lightweight).
    err = Backend::connect();
    if (err != ESP_OK) {
      rollback_();
      return err;
    }

    return ESP_OK;
  }

  esp_err_t end() noexcept {
    // Mark inactive first, so callbacks become no-ops even if events arrive briefly.
    begun_ = false;

    // Stop retry timer (best-effort, no blocking)
    stop_retry_timer_();

    // Disconnect/stop (best-effort; ignore "not started" style errors)
    (void)Backend::disconnect();
    (void)Backend::stop();

    // Unregister handlers (deterministic lifecycle)
    if (handlers_registered_) {
      (void)Backend::unregister_handlers(wifi_handler_, ip_handler_);
    }
    handlers_registered_ = false;
    wifi_handler_ = nullptr;
    ip_handler_ = nullptr;

    // Deinit wifi driver
    if (wifi_inited_) {
      (void)Backend::deinit_wifi();
    }
    wifi_inited_ = false;
    wifi_started_ = false;

    // Clear flags + bits
    clear_all_state_();
    return ESP_OK;
  }

  bool has_ip() const noexcept { return has_ip_; }
  bool is_wifi_up() const noexcept { return wifi_up_; }
  int last_disconnect_reason() const noexcept { return last_reason_; }
  bool needs_manual_intervention() const noexcept { return needs_manual_intervention_; }

  // Wait until we have a valid IP (GOT_IP and not LOST_IP)
  bool wait_has_ip(TickType_t timeout_ticks) noexcept {
    if (event_group_ == nullptr) return false;
    const EventBits_t bits =
        xEventGroupWaitBits(event_group_, kBitHasIp, pdFALSE /*clearOnExit*/, pdTRUE /*waitAll*/, timeout_ticks);
    return (bits & kBitHasIp) != 0;
  }

  // Force a connect attempt (if allowed)
  esp_err_t request_connect() noexcept {
    if (!wifi_inited_ || !wifi_started_) return ESP_ERR_INVALID_STATE;

    // Manual intervention resets retry state
    needs_manual_intervention_ = false;
    attempt_ = 0;

    // Cancel any pending retry timer
    stop_retry_timer_();

    return Backend::connect();
  }

  // Explicit disconnect request
  esp_err_t request_disconnect() noexcept {
    stop_retry_timer_();
    return Backend::disconnect();
  }

 private:
  // -----------------------------
  // EventGroup bits
  // -----------------------------
  static constexpr EventBits_t kBitHasIp = (1U << 0);
  static constexpr EventBits_t kBitConnected = (1U << 1);
  static constexpr EventBits_t kBitNeedsManual = (1U << 2);
  static constexpr EventBits_t kBitWifiUp = (1U << 3);
  static constexpr EventBits_t kBitsMask = (kBitHasIp | kBitConnected | kBitNeedsManual | kBitWifiUp);

  // -----------------------------
  // Static callback trampolines
  // -----------------------------
  static void ctx_on_sta_start_(void* self) noexcept {
    static_cast<WifiStaManager*>(self)->on_sta_start_();
  }
  static void ctx_on_sta_disconnected_(void* self, int reason) noexcept {
    static_cast<WifiStaManager*>(self)->on_sta_disconnected_(reason);
  }
  static void ctx_on_got_ip_(void* self, const esp_netif_ip_info_t* ip) noexcept {
    static_cast<WifiStaManager*>(self)->on_got_ip_(ip);
  }
  static void ctx_on_lost_ip_(void* self) noexcept {
    static_cast<WifiStaManager*>(self)->on_lost_ip_();
  }

  // -----------------------------
  // Internal event handling (lightweight)
  // -----------------------------
  void on_sta_start_() noexcept {
    if (!begun_) return;
    wifi_up_ = true;
    if (event_group_ != nullptr) {
      (void)xEventGroupSetBits(event_group_, kBitWifiUp);
    }
  }

  void on_sta_disconnected_(int reason) noexcept {
    if (!begun_) return;

    // Clear IP / connected state
    has_ip_ = false;
    if (event_group_ != nullptr) {
      (void)xEventGroupClearBits(event_group_, kBitHasIp | kBitConnected);
    }

    last_reason_ = reason;
    notifier_.on_disconnected(reason);

    // Decide retry
    if (RetryPolicy::is_permanent(reason)) {
      needs_manual_intervention_ = true;
      if (event_group_ != nullptr) {
        (void)xEventGroupSetBits(event_group_, kBitNeedsManual);
      }
      return;
    }

    // attempt_ counts retries (1..N)
    const int next_attempt = attempt_ + 1;
    if (!RetryPolicy::should_retry(reason, next_attempt)) {
      needs_manual_intervention_ = true;
      if (event_group_ != nullptr) {
        (void)xEventGroupSetBits(event_group_, kBitNeedsManual);
      }
      return;
    }

    attempt_ = next_attempt;
    schedule_retry_(RetryPolicy::backoff_ticks(attempt_));
  }

  void on_got_ip_(const esp_netif_ip_info_t* ip) noexcept {
    if (!begun_) return;

    has_ip_ = true;
    needs_manual_intervention_ = false;
    attempt_ = 0;

    if (event_group_ != nullptr) {
      (void)xEventGroupSetBits(event_group_, kBitHasIp | kBitConnected);
      (void)xEventGroupClearBits(event_group_, kBitNeedsManual);
    }

    if (ip != nullptr) {
      notifier_.on_got_ip(*ip);
    }
    notifier_.on_connected();
  }

  void on_lost_ip_() noexcept {
    if (!begun_) return;

    has_ip_ = false;
    if (event_group_ != nullptr) {
      (void)xEventGroupClearBits(event_group_, kBitHasIp);
    }
    notifier_.on_lost_ip();
  }

  // -----------------------------
  // Retry timer (one-shot)
  // -----------------------------
  static void retry_timer_cb_(TimerHandle_t xTimer) {
    void* const id = pvTimerGetTimerID(xTimer);
    if (id != nullptr) {
      static_cast<WifiStaManager*>(id)->on_retry_timer_();
    }
  }

  void on_retry_timer_() noexcept {
    // No heavy work; a single esp_wifi_connect() call is fine.
    if (!begun_) return;
    if (!wifi_inited_ || !wifi_started_) return;
    if (needs_manual_intervention_) return;

    (void)Backend::connect();
  }

  void schedule_retry_(TickType_t delay_ticks) noexcept {
    if (retry_timer_ == nullptr) return;

    // Best-effort: stop + (re)set period + start
    (void)xTimerStop(retry_timer_, 0);
    (void)xTimerChangePeriod(retry_timer_, delay_ticks, 0);
    (void)xTimerStart(retry_timer_, 0);
  }

  void stop_retry_timer_() noexcept {
    if (retry_timer_ == nullptr) return;
    (void)xTimerStop(retry_timer_, 0);
  }

  // -----------------------------
  // Creation / reset helpers
  // -----------------------------
  void ensure_event_group_created_() noexcept {
    if (event_group_ == nullptr) {
      event_group_ = xEventGroupCreateStatic(&event_group_buf_);
    }
    if (event_group_ != nullptr) {
      (void)xEventGroupClearBits(event_group_, kBitsMask);
    }
  }

  void ensure_timer_created_() noexcept {
    if (retry_timer_ == nullptr) {
      retry_timer_ = xTimerCreateStatic(
          "wifi_retry",
          pdMS_TO_TICKS(1000) /*dummy*/,
          pdFALSE /*one-shot*/,
          static_cast<void*>(this) /*timer ID*/,
          &WifiStaManager::retry_timer_cb_,
          &retry_timer_buf_);
    } else {
      // keep the same timer id (this)
      vTimerSetTimerID(retry_timer_, static_cast<void*>(this));
    }
    stop_retry_timer_();
  }

  void clear_all_state_() noexcept {
    has_ip_ = false;
    wifi_up_ = false;
    wifi_started_ = false;
    wifi_inited_ = false;
    handlers_registered_ = false;
    needs_manual_intervention_ = false;
    attempt_ = 0;
    last_reason_ = -1;
    sta_netif_ = nullptr;
    wifi_handler_ = nullptr;
    ip_handler_ = nullptr;

    if (event_group_ != nullptr) {
      (void)xEventGroupClearBits(event_group_, kBitHasIp | kBitConnected | kBitNeedsManual | kBitWifiUp);
    }
  }

  void rollback_() noexcept {
    // Called on begin() error path; must leave system clean (no handlers registered, no timer active).
    begun_ = false;
    stop_retry_timer_();

    (void)Backend::disconnect();
    (void)Backend::stop();

    if (handlers_registered_) {
      (void)Backend::unregister_handlers(wifi_handler_, ip_handler_);
    }
    handlers_registered_ = false;
    wifi_handler_ = nullptr;
    ip_handler_ = nullptr;

    if (wifi_inited_) {
      (void)Backend::deinit_wifi();
    }

    wifi_inited_ = false;
    wifi_started_ = false;
    clear_all_state_();
  }

 private:
  // State (minimal)
  bool begun_ = false;
  bool has_ip_ = false;
  bool wifi_up_ = false;
  bool wifi_started_ = false;
  bool wifi_inited_ = false;
  bool handlers_registered_ = false;
  bool needs_manual_intervention_ = false;
  int attempt_ = 0;
  int last_reason_ = -1;

  // RTOS primitives (static, no heap)
  StaticEventGroup_t event_group_buf_{};
  EventGroupHandle_t event_group_ = nullptr;

  StaticTimer_t retry_timer_buf_{};
  TimerHandle_t retry_timer_ = nullptr;

  // Netif (not owned by us; created/reused by backend)
  esp_netif_t* sta_netif_ = nullptr;

  // Handler ctx and opaque handles
  HandlerCtx handler_ctx_{};
  typename Backend::handler_handle_t wifi_handler_ = nullptr;
  typename Backend::handler_handle_t ip_handler_ = nullptr;

  // Notifier instance (no heap, no virtual)
  Notifier notifier_{};
};

}  // namespace wifi_sta
