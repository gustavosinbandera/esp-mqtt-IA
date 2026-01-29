#include <cstdint>
#include <cstring>
#include <cstdio>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "wifi_sta/wifi_sta.hpp"
#include "mqtt_client/mqtt_client.hpp"

static const char* kTag = "app_main";

// --- Wi-Fi credentials (compile-time demo) ---
struct MyCredentials {
  static const char* ssid() noexcept { return "esp32"; }
  static const char* password() noexcept { return "12345678"; }
};

using WifiMgr = wifi_sta::WifiStaManager<
    wifi_sta::EspIdfWifiBackend,
    MyCredentials,
    wifi_sta::DefaultRetryPolicy,
    wifi_sta::EventGroupNotifier>;

static void on_mqtt_message(void* /*ctx*/, const mqtt_client::MessageView& msg) noexcept {
  ESP_LOGI("mqtt_rx", "topic=%.*s len=%u qos=%d retain=%d",
           (int)msg.topic_len, msg.topic,
           (unsigned)msg.payload_len,
           (int)msg.qos,
           (int)msg.retain);

  char buf[128];
  const std::size_t n = (msg.payload_len < (sizeof(buf) - 1)) ? msg.payload_len : (sizeof(buf) - 1);
  if (n > 0 && msg.payload) {
    std::memcpy(buf, msg.payload, n);
  }
  buf[n] = '\0';
  ESP_LOGI("mqtt_rx", "payload='%s'", buf);
}

// Minimal mapping for common Wi-Fi disconnect reasons seen during debugging
static const char* wifi_reason_hint(int reason) noexcept {
  switch (reason) {
    case 201: return "NO_AP_FOUND (SSID no visible / 5GHz / channel unsupported / too far)";
    case 202: return "AUTH_FAIL (password wrong / WPA3-only / auth mismatch)";
    case 203: return "ASSOC_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT";
    default:  return "";
  }
}

extern "C" void app_main(void) {
  ESP_LOGI(kTag, "Boot");

  // NVS init
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    (void)nvs_flash_erase();
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "nvs_flash_init failed: %s", esp_err_to_name(err));
    return;
  }

  static WifiMgr wifi;

  err = wifi.begin(nullptr);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "wifi.begin failed: %s", esp_err_to_name(err));
    return;
  }

  // Wait for IP (up to 30s) + log disconnect reason only when it changes
  ESP_LOGI(kTag, "Waiting for IP (up to 30s)...");
  const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(30000);

  int last_reason = -1;

  while (xTaskGetTickCount() < deadline) {
    if (wifi.has_ip()) {
      ESP_LOGI(kTag, "Wi-Fi has IP!");
      break;
    }

    const int reason = wifi.last_disconnect_reason();
    if (reason != -1 && reason != last_reason) {
      ESP_LOGW(kTag, "Wi-Fi disconnect reason=%d %s", reason, wifi_reason_hint(reason));
      last_reason = reason;
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  if (!wifi.has_ip()) {
    ESP_LOGE(kTag, "No IP after 30s. MQTT will not start.");
    return;
  }

  ESP_LOGI(kTag, "Wi-Fi has IP. Starting MQTT...");

  static mqtt_client::Client mqtt;

  err = mqtt.init(nullptr);  // uses sdkconfig defaults
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "mqtt.init failed: %s", esp_err_to_name(err));
    return;
  }

  mqtt.set_message_callback(&on_mqtt_message, nullptr);

  err = mqtt.start();
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "mqtt.start failed: %s", esp_err_to_name(err));
    return;
  }

  (void)mqtt.subscribe("demo/rx", 0);

  int counter = 0;
  while (true) {
    char msg[64];
    ::snprintf(msg, sizeof(msg), "hello %d", counter++);
    (void)mqtt.publish_str("demo/tx", msg, 0, false);

    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}
