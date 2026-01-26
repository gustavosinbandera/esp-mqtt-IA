#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "esp_event_base.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"

namespace mqtt_client {

struct MessageView {
    const char* topic = nullptr;
    std::size_t topic_len = 0;

    const std::uint8_t* payload = nullptr;
    std::size_t payload_len = 0;

    int qos = 0;
    bool retain = false;
    bool dup = false;
};

using MessageCallback = void (*)(void* user_ctx, const MessageView& msg) noexcept;

struct LwtConfig {
    const char* topic = nullptr;   // nullptr => disable (unless enabled by sdkconfig)
    const char* message = nullptr;
    int qos = 1;
    bool retain = true;
};

struct Config {
    const char* broker_uri = nullptr;
    const char* client_id = nullptr;
    const char* username = nullptr;
    const char* password = nullptr;

    int keepalive_sec = 0;          // <=0 => sdkconfig
    bool clean_session = true;

    std::uint32_t backoff_min_ms = 0; // 0 => sdkconfig
    std::uint32_t backoff_max_ms = 0; // 0 => sdkconfig

    const LwtConfig* lwt = nullptr;   // nullptr => sdkconfig (or disabled)
};

class Client final {
public:
    Client() = default;

    esp_err_t init(const Config* cfg = nullptr) noexcept;
    esp_err_t start() noexcept;   // assumes network ready (Wi-Fi has IP)
    esp_err_t stop() noexcept;

    bool is_connected() const noexcept;
    int last_disconnect_reason() const noexcept;

    void set_message_callback(MessageCallback cb, void* user_ctx) noexcept;

    esp_err_t publish(const char* topic,
                      const void* payload,
                      std::size_t len,
                      int qos,
                      bool retain,
                      TickType_t timeout_ticks = 0) noexcept;

    esp_err_t publish_str(const char* topic,
                          const char* payload,
                          int qos,
                          bool retain,
                          TickType_t timeout_ticks = 0) noexcept;

    esp_err_t subscribe(const char* topic, int qos, TickType_t timeout_ticks = 0) noexcept;

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

private:
    // ---- Sizes from sdkconfig ----
    static constexpr std::size_t kMaxTopicLen = static_cast<std::size_t>(CONFIG_MQTT_CLIENT_MAX_TOPIC_LEN);
    static constexpr std::size_t kMaxPayloadLen = static_cast<std::size_t>(CONFIG_MQTT_CLIENT_MAX_PAYLOAD_LEN);

    static constexpr std::size_t kCtrlStackWords =
        (static_cast<std::size_t>(CONFIG_MQTT_CLIENT_CTRL_TASK_STACK_SIZE) + sizeof(StackType_t) - 1) / sizeof(StackType_t);

    static constexpr std::size_t kRxStackWords =
        (static_cast<std::size_t>(CONFIG_MQTT_CLIENT_RX_TASK_STACK_SIZE) + sizeof(StackType_t) - 1) / sizeof(StackType_t);

    // ---- RX item ----
    struct RxItem {
        char topic[kMaxTopicLen];
        std::uint8_t payload[kMaxPayloadLen];
        std::uint16_t topic_len;
        std::uint16_t payload_len;
        std::int8_t qos;
        std::uint8_t retain;
        std::uint8_t dup;
    };

    // ---- Outbox item ----
    struct OutboxItem {
        char topic[kMaxTopicLen];
        std::uint8_t payload[kMaxPayloadLen];
        std::uint16_t topic_len;
        std::uint16_t payload_len;
        std::int8_t qos;
        std::uint8_t retain;
    };

    enum class CtrlType : std::uint8_t {
        Work = 0,
        Disconnected,
        RetryNow
    };

    struct CtrlMsg {
        CtrlType type;
    };

    static constexpr EventBits_t BIT_INITIALIZED = (1U << 0);
    static constexpr EventBits_t BIT_STARTED     = (1U << 1);
    static constexpr EventBits_t BIT_CONNECTED   = (1U << 2);

    static void ctrl_task_entry(void* arg) noexcept;
    static void rx_task_entry(void* arg) noexcept;
    static void retry_timer_cb(TimerHandle_t tmr) noexcept;

    // esp-mqtt event handler (static member: can access private state safely)
    static void mqtt_event_handler(void* handler_args,
                                   esp_event_base_t base,
                                   int32_t event_id,
                                   void* event_data) ;

    // ---- Config storage ----
    char broker_uri_[kMaxTopicLen];
    char client_id_[kMaxTopicLen];
    char username_[kMaxTopicLen];
    char password_[kMaxTopicLen];

    bool clean_session_ = true;
    int keepalive_sec_ = 30;
    std::uint32_t backoff_min_ms_ = 500;
    std::uint32_t backoff_max_ms_ = 30000;

    bool lwt_enabled_ = false;
    char lwt_topic_[kMaxTopicLen];
    char lwt_msg_[kMaxPayloadLen];
    int lwt_qos_ = 1;
    bool lwt_retain_ = true;

    void* mqtt_client_ = nullptr;

    int last_disconnect_reason_ = 0;
    std::uint32_t backoff_attempt_ = 0;

    MessageCallback user_cb_ = nullptr;
    void* user_cb_ctx_ = nullptr;

    StaticEventGroup_t eg_buf_;
    EventGroupHandle_t eg_ = nullptr;

    StaticSemaphore_t mutex_buf_;
    SemaphoreHandle_t mutex_ = nullptr;

    StaticQueue_t rx_q_buf_;
    QueueHandle_t rx_q_ = nullptr;
    std::uint8_t rx_q_storage_[CONFIG_MQTT_CLIENT_RX_QUEUE_LEN * sizeof(RxItem)];

    StaticQueue_t ctrl_q_buf_;
    QueueHandle_t ctrl_q_ = nullptr;
    std::uint8_t ctrl_q_storage_[CONFIG_MQTT_CLIENT_CTRL_QUEUE_LEN * sizeof(CtrlMsg)];

    StaticTimer_t retry_timer_buf_;
    TimerHandle_t retry_timer_ = nullptr;

    StaticTask_t ctrl_tcb_;
    StackType_t ctrl_stack_[kCtrlStackWords];

    StaticTask_t rx_tcb_;
    StackType_t rx_stack_[kRxStackWords];

    static constexpr std::size_t kOutboxLen = static_cast<std::size_t>(CONFIG_MQTT_CLIENT_OUTBOX_LEN);
    OutboxItem outbox_[ (kOutboxLen > 0) ? kOutboxLen : 1 ];
    std::size_t outbox_head_ = 0;
    std::size_t outbox_tail_ = 0;
    std::size_t outbox_count_ = 0;

    static constexpr std::size_t kSubMax = 16;
    struct SubItem {
        char topic[kMaxTopicLen];
        std::uint16_t topic_len;
        std::int8_t qos;
        bool used;
    };
    SubItem subs_[kSubMax];

    void load_config_(const Config* cfg) noexcept;
    void build_and_init_mqtt_() noexcept;

    bool outbox_push_(const char* topic, const void* payload, std::size_t len, int qos, bool retain) noexcept;
    bool outbox_peek_(OutboxItem& out) const noexcept;
    void outbox_pop_() noexcept;
    void outbox_clear_() noexcept;

    bool subs_upsert_(const char* topic, int qos) noexcept;
    void subs_clear_() noexcept;

    void request_work_() noexcept;
    void request_disconnected_() noexcept;
};

} // namespace mqtt_client
