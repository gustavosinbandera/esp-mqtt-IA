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
  static const char* ssid() noexcept { return "invitados"; }
  static const char* password() noexcept { return "12345678"; }
};

using WifiMgr = wifi_sta::WifiStaManager<
    wifi_sta::EspIdfWifiBackend,
    MyCredentials,
    wifi_sta::DefaultRetryPolicy,
    wifi_sta::EventGroupNotifier>;

static void on_mqtt_message(void* /*ctx*/, const mqtt_client::MessageView& msg) noexcept {
  // This callback runs in the MQTT component RX task (NOT in the esp-mqtt event handler).
  // Keep it reasonably fast; if you need heavy work, queue it to your own pipeline.
  ESP_LOGI("mqtt_rx", "topic=%.*s len=%u qos=%d retain=%d",
           (int)msg.topic_len, msg.topic,
           (unsigned)msg.payload_len,
           msg.qos,
           (int)msg.retain);

  // Example: print payload as string (safe copy to a small stack buffer)
  char buf[128];
  const std::size_t n = (msg.payload_len < (sizeof(buf) - 1)) ? msg.payload_len : (sizeof(buf) - 1);
  if (n > 0 && msg.payload) {
    std::memcpy(buf, msg.payload, n);
  }
  buf[n] = '\0';
  ESP_LOGI("mqtt_rx", "payload='%s'", buf);
}

extern "C" void app_main(void) {
  // NVS init (common requirement for Wi-Fi)
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

  // Wait for IP before starting MQTT (contract: mqtt.start() only when network ready)
  const bool got_ip = wifi.wait_has_ip(pdMS_TO_TICKS(15000));
  if (!got_ip) {
    ESP_LOGW(kTag, "No IP yet (timeout). MQTT will not start.");
    return;
  }
  ESP_LOGI(kTag, "Wi-Fi has IP. Starting MQTT...");

  static mqtt_client::Client mqtt;
    err = mqtt.init(nullptr); // uses sdkconfig defaults
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
    // Publish a heartbeat every 2 seconds
    char msg[64];
    ::snprintf(msg, sizeof(msg), "hello %d", counter++);
    (void)mqtt.publish_str("demo/tx", msg, 0, false);

    if (!mqtt.is_connected()) {
      const int reason = mqtt.last_disconnect_reason();
      ESP_LOGW(kTag, "MQTT not connected. last_disconnect_reason=%d", reason);
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}
