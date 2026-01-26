#include "mqtt_client/mqtt_client.hpp"

extern "C" {
#include "mqtt_client.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_random.h"
}

#include <cstring>

namespace mqtt_client {

static const char* kTag = "mqtt_client";

static inline void lock(SemaphoreHandle_t m) noexcept {
    if (m) { (void)xSemaphoreTake(m, portMAX_DELAY); }
}

static inline void unlock(SemaphoreHandle_t m) noexcept {
    if (m) { (void)xSemaphoreGive(m); }
}

static inline void safe_copy_cstr(char* dst, std::size_t dst_cap, const char* src) noexcept {
    if (!dst || dst_cap == 0) { return; }
    if (!src) { dst[0] = '\0'; return; }
    std::size_t n = std::strlen(src);
    if (n >= dst_cap) { n = dst_cap - 1; }
    std::memcpy(dst, src, n);
    dst[n] = '\0';
}

static int extract_reason_idf54(const esp_mqtt_event_handle_t event) noexcept {
    if (!event || !event->error_handle) {
        return 0;
    }
    const esp_mqtt_error_codes_t* e = event->error_handle;

    // IDF 5.4-safe fields (as requested):
    // - esp_tls_last_esp_err
    // - esp_transport_sock_errno
    // - error_type
    // - connect_return_code (when applies)
    if (e->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
        return static_cast<int>(e->connect_return_code);
    }

    if (e->esp_tls_last_esp_err != 0) {
        return static_cast<int>(e->esp_tls_last_esp_err);
    }

    if (e->esp_transport_sock_errno != 0) {
        return -static_cast<int>(e->esp_transport_sock_errno);
    }

    return 0;
}

void Client::mqtt_event_handler(void* handler_args,
                                esp_event_base_t /*base*/,
                                int32_t event_id,
                                void* event_data) {
    Client* self = static_cast<Client*>(handler_args);
    if (!self) {
        return;
    }

    esp_mqtt_event_handle_t event = static_cast<esp_mqtt_event_handle_t>(event_data);
    const esp_mqtt_event_id_t id = static_cast<esp_mqtt_event_id_t>(event_id);

    switch (id) {
        case MQTT_EVENT_CONNECTED:
            xEventGroupSetBits(self->eg_, BIT_CONNECTED);
            self->backoff_attempt_ = 0;
            self->request_work_();
            ESP_LOGI(kTag, "MQTT connected");
            break;

        case MQTT_EVENT_DISCONNECTED:
            xEventGroupClearBits(self->eg_, BIT_CONNECTED);
            self->request_disconnected_();
            ESP_LOGW(kTag, "MQTT disconnected");
            break;

        case MQTT_EVENT_ERROR:
            self->last_disconnect_reason_ = extract_reason_idf54(event);
            ESP_LOGW(kTag, "MQTT error, reason=%d", self->last_disconnect_reason_);
            break;

        case MQTT_EVENT_DATA: {
            if (!event || !event->topic || event->topic_len <= 0) {
                break;
            }

            // Copy topic + payload into fixed RxItem then enqueue (drop if full).
            if (static_cast<std::size_t>(event->topic_len) >= kMaxTopicLen) {
                break;
            }
            if (event->data_len > 0 && static_cast<std::size_t>(event->data_len) > kMaxPayloadLen) {
                break;
            }

            RxItem item{};
            item.topic_len = static_cast<std::uint16_t>(event->topic_len);
            std::memcpy(item.topic, event->topic, static_cast<std::size_t>(event->topic_len));
            item.topic[item.topic_len] = '\0';

            if (event->data_len > 0) {
                item.payload_len = static_cast<std::uint16_t>(event->data_len);
                std::memcpy(item.payload, event->data, static_cast<std::size_t>(event->data_len));
            } else {
                item.payload_len = 0;
            }

            item.qos = static_cast<std::int8_t>(event->qos);
            item.retain = static_cast<std::uint8_t>(event->retain ? 1 : 0);
            item.dup = static_cast<std::uint8_t>(event->dup ? 1 : 0);

            (void)xQueueSend(self->rx_q_, &item, 0);
            break;
        }

        default:
            break;
    }
}

esp_err_t Client::init(const Config* cfg) noexcept {
    if (eg_ && ((xEventGroupGetBits(eg_) & BIT_INITIALIZED) != 0)) {
        return ESP_OK;
    }

    eg_ = xEventGroupCreateStatic(&eg_buf_);
    if (!eg_) {
        return ESP_ERR_NO_MEM;
    }

    mutex_ = xSemaphoreCreateMutexStatic(&mutex_buf_);
    if (!mutex_) {
        return ESP_ERR_NO_MEM;
    }

    rx_q_ = xQueueCreateStatic(CONFIG_MQTT_CLIENT_RX_QUEUE_LEN,
                              sizeof(RxItem),
                              rx_q_storage_,
                              &rx_q_buf_);
    if (!rx_q_) {
        return ESP_ERR_NO_MEM;
    }

    ctrl_q_ = xQueueCreateStatic(CONFIG_MQTT_CLIENT_CTRL_QUEUE_LEN,
                                sizeof(CtrlMsg),
                                ctrl_q_storage_,
                                &ctrl_q_buf_);
    if (!ctrl_q_) {
        return ESP_ERR_NO_MEM;
    }

    retry_timer_ = xTimerCreateStatic("mqtt_retry",
                                      pdMS_TO_TICKS(1000),
                                      pdFALSE,
                                      this,
                                      &Client::retry_timer_cb,
                                      &retry_timer_buf_);
    if (!retry_timer_) {
        return ESP_ERR_NO_MEM;
    }

    subs_clear_();
    load_config_(cfg);

    (void)xTaskCreateStatic(&Client::ctrl_task_entry,
                           "mqtt_ctrl",
                           static_cast<uint32_t>(kCtrlStackWords),
                           this,
                           CONFIG_MQTT_CLIENT_TASK_PRIORITY,
                           ctrl_stack_,
                           &ctrl_tcb_);

    (void)xTaskCreateStatic(&Client::rx_task_entry,
                           "mqtt_rx",
                           static_cast<uint32_t>(kRxStackWords),
                           this,
                           CONFIG_MQTT_CLIENT_TASK_PRIORITY,
                           rx_stack_,
                           &rx_tcb_);

    xEventGroupSetBits(eg_, BIT_INITIALIZED);
    return ESP_OK;
}

void Client::load_config_(const Config* cfg) noexcept {
    safe_copy_cstr(broker_uri_, sizeof(broker_uri_), CONFIG_MQTT_CLIENT_BROKER_URI);
    safe_copy_cstr(client_id_, sizeof(client_id_), CONFIG_MQTT_CLIENT_CLIENT_ID);
    safe_copy_cstr(username_, sizeof(username_), CONFIG_MQTT_CLIENT_USERNAME);
    safe_copy_cstr(password_, sizeof(password_), CONFIG_MQTT_CLIENT_PASSWORD);

    clean_session_ = CONFIG_MQTT_CLIENT_CLEAN_SESSION;
    keepalive_sec_ = CONFIG_MQTT_CLIENT_KEEPALIVE_SEC;
    backoff_min_ms_ = static_cast<std::uint32_t>(CONFIG_MQTT_CLIENT_BACKOFF_MIN_MS);
    backoff_max_ms_ = static_cast<std::uint32_t>(CONFIG_MQTT_CLIENT_BACKOFF_MAX_MS);

    lwt_enabled_ = false;
    lwt_topic_[0] = '\0';
    lwt_msg_[0] = '\0';
    lwt_qos_ = 1;
    lwt_retain_ = true;

#if CONFIG_MQTT_CLIENT_LWT_ENABLE
    lwt_enabled_ = true;
    safe_copy_cstr(lwt_topic_, sizeof(lwt_topic_), CONFIG_MQTT_CLIENT_LWT_TOPIC);
    safe_copy_cstr(lwt_msg_, sizeof(lwt_msg_), CONFIG_MQTT_CLIENT_LWT_MSG);
    lwt_qos_ = CONFIG_MQTT_CLIENT_LWT_QOS;
    lwt_retain_ = CONFIG_MQTT_CLIENT_LWT_RETAIN;
#endif

    if (cfg) {
        if (cfg->broker_uri) { safe_copy_cstr(broker_uri_, sizeof(broker_uri_), cfg->broker_uri); }
        if (cfg->client_id)  { safe_copy_cstr(client_id_,  sizeof(client_id_),  cfg->client_id); }
        if (cfg->username)   { safe_copy_cstr(username_,   sizeof(username_),   cfg->username); }
        if (cfg->password)   { safe_copy_cstr(password_,   sizeof(password_),   cfg->password); }

        if (cfg->keepalive_sec > 0) {
            keepalive_sec_ = cfg->keepalive_sec;
        }
        clean_session_ = cfg->clean_session;

        if (cfg->backoff_min_ms != 0) { backoff_min_ms_ = cfg->backoff_min_ms; }
        if (cfg->backoff_max_ms != 0) { backoff_max_ms_ = cfg->backoff_max_ms; }

        if (cfg->lwt) {
            if (cfg->lwt->topic) {
                lwt_enabled_ = true;
                safe_copy_cstr(lwt_topic_, sizeof(lwt_topic_), cfg->lwt->topic);
                safe_copy_cstr(lwt_msg_, sizeof(lwt_msg_), cfg->lwt->message ? cfg->lwt->message : "");
                lwt_qos_ = cfg->lwt->qos;
                lwt_retain_ = cfg->lwt->retain;
            } else {
                lwt_enabled_ = false;
            }
        }
    }

    if (backoff_min_ms_ > backoff_max_ms_) {
        backoff_min_ms_ = backoff_max_ms_;
    }
}

void Client::build_and_init_mqtt_() noexcept {
    if (mqtt_client_ != nullptr) {
        return;
    }

    esp_mqtt_client_config_t mqtt_cfg{};
    mqtt_cfg.broker.address.uri = broker_uri_;

    mqtt_cfg.credentials.client_id = client_id_;
    if (username_[0] != '\0') {
        mqtt_cfg.credentials.username = username_;
    }
    if (password_[0] != '\0') {
        mqtt_cfg.credentials.authentication.password = password_;
    }

    mqtt_cfg.session.keepalive = keepalive_sec_;
    mqtt_cfg.session.disable_clean_session = !clean_session_;

    // We do our own reconnect logic (timer + backoff)
    mqtt_cfg.network.disable_auto_reconnect = true;

    if (lwt_enabled_ && lwt_topic_[0] != '\0') {
        mqtt_cfg.session.last_will.topic = lwt_topic_;
        mqtt_cfg.session.last_will.msg = lwt_msg_;
        mqtt_cfg.session.last_will.msg_len = static_cast<int>(std::strlen(lwt_msg_));
        mqtt_cfg.session.last_will.qos = lwt_qos_;
        mqtt_cfg.session.last_will.retain = lwt_retain_;
    }

    esp_mqtt_client_handle_t h = esp_mqtt_client_init(&mqtt_cfg);
    if (!h) {
        ESP_LOGE(kTag, "esp_mqtt_client_init failed");
        return;
    }
    mqtt_client_ = static_cast<void*>(h);

    (void)esp_mqtt_client_register_event(h, MQTT_EVENT_ANY, &Client::mqtt_event_handler, this);
}

esp_err_t Client::start() noexcept {
    if (!eg_ || ((xEventGroupGetBits(eg_) & BIT_INITIALIZED) == 0)) {
        return ESP_ERR_INVALID_STATE;
    }

    lock(mutex_);

    if ((xEventGroupGetBits(eg_) & BIT_STARTED) != 0) {
        unlock(mutex_);
        return ESP_OK;
    }

    build_and_init_mqtt_();
    if (!mqtt_client_) {
        unlock(mutex_);
        return ESP_FAIL;
    }

    last_disconnect_reason_ = 0;
    backoff_attempt_ = 0;
    outbox_clear_();

    (void)xTimerStop(retry_timer_, 0);

    esp_mqtt_client_handle_t h = static_cast<esp_mqtt_client_handle_t>(mqtt_client_);
    const esp_err_t err = esp_mqtt_client_start(h);
    if (err == ESP_OK) {
        xEventGroupSetBits(eg_, BIT_STARTED);
    }

    unlock(mutex_);
    return err;
}

esp_err_t Client::stop() noexcept {
    if (!eg_ || ((xEventGroupGetBits(eg_) & BIT_INITIALIZED) == 0)) {
        return ESP_ERR_INVALID_STATE;
    }

    lock(mutex_);

    xEventGroupClearBits(eg_, BIT_STARTED);
    xEventGroupClearBits(eg_, BIT_CONNECTED);

    (void)xTimerStop(retry_timer_, 0);

    if (mqtt_client_) {
        esp_mqtt_client_handle_t h = static_cast<esp_mqtt_client_handle_t>(mqtt_client_);
        (void)esp_mqtt_client_stop(h);
        (void)esp_mqtt_client_destroy(h);
        mqtt_client_ = nullptr;
    }

    outbox_clear_();

    unlock(mutex_);
    return ESP_OK;
}

bool Client::is_connected() const noexcept {
    if (!eg_) { return false; }
    return (xEventGroupGetBits(eg_) & BIT_CONNECTED) != 0;
}

int Client::last_disconnect_reason() const noexcept {
    return last_disconnect_reason_;
}

void Client::set_message_callback(MessageCallback cb, void* user_ctx) noexcept {
    lock(mutex_);
    user_cb_ = cb;
    user_cb_ctx_ = user_ctx;
    unlock(mutex_);
}

void Client::request_work_() noexcept {
    if (!ctrl_q_) { return; }
    const CtrlMsg msg{ CtrlType::Work };
    (void)xQueueSend(ctrl_q_, &msg, 0);
}

void Client::request_disconnected_() noexcept {
    if (!ctrl_q_) { return; }
    const CtrlMsg msg{ CtrlType::Disconnected };
    (void)xQueueSend(ctrl_q_, &msg, 0);
}

static std::uint32_t compute_backoff_with_jitter(std::uint32_t min_ms,
                                                 std::uint32_t max_ms,
                                                 std::uint32_t attempt) noexcept {
    if (min_ms == 0) { min_ms = 100; }
    if (max_ms < min_ms) { max_ms = min_ms; }

    std::uint64_t base = static_cast<std::uint64_t>(min_ms);
    const std::uint32_t capped = (attempt > 20U) ? 20U : attempt;
    base <<= capped;
    if (base > max_ms) { base = max_ms; }

    const std::uint32_t base_u32 = static_cast<std::uint32_t>(base);
    const std::uint32_t jitter_max = (base_u32 / 4U) + 1U;
    const std::uint32_t jitter = (jitter_max > 1U) ? (::esp_random() % jitter_max) : 0U;

    std::uint64_t delay = static_cast<std::uint64_t>(base_u32) + jitter;
    if (delay > max_ms) { delay = max_ms; }
    if (delay < min_ms) { delay = min_ms; }
    return static_cast<std::uint32_t>(delay);
}

void Client::retry_timer_cb(TimerHandle_t tmr) noexcept {
    Client* self = static_cast<Client*>(pvTimerGetTimerID(tmr));
    if (!self || !self->ctrl_q_) {
        return;
    }
    const CtrlMsg msg{ CtrlType::RetryNow };
    (void)xQueueSend(self->ctrl_q_, &msg, 0);
}

bool Client::outbox_push_(const char* topic, const void* payload, std::size_t len, int qos, bool retain) noexcept {
    if (kOutboxLen == 0) {
        return false;
    }
    if (!topic) {
        return false;
    }
    const std::size_t tlen = std::strlen(topic);
    if (tlen == 0 || tlen >= kMaxTopicLen) {
        return false;
    }
    if (len > kMaxPayloadLen) {
        return false;
    }

    if (outbox_count_ >= kOutboxLen) {
#if CONFIG_MQTT_CLIENT_OUTBOX_OVERWRITE_OLDEST
        outbox_pop_();
#else
        return false;
#endif
    }

    OutboxItem& dst = outbox_[outbox_tail_];
    std::memcpy(dst.topic, topic, tlen);
    dst.topic[tlen] = '\0';
    dst.topic_len = static_cast<std::uint16_t>(tlen);

    if (len > 0 && payload) {
        std::memcpy(dst.payload, payload, len);
    }
    dst.payload_len = static_cast<std::uint16_t>(len);

    if (qos < 0) qos = 0;
    if (qos > 2) qos = 2;
    dst.qos = static_cast<std::int8_t>(qos);
    dst.retain = static_cast<std::uint8_t>(retain ? 1 : 0);

    outbox_tail_ = (outbox_tail_ + 1) % kOutboxLen;
    outbox_count_++;
    return true;
}

bool Client::outbox_peek_(OutboxItem& out) const noexcept {
    if (kOutboxLen == 0 || outbox_count_ == 0) {
        return false;
    }
    out = outbox_[outbox_head_];
    return true;
}

void Client::outbox_pop_() noexcept {
    if (kOutboxLen == 0 || outbox_count_ == 0) {
        return;
    }
    outbox_head_ = (outbox_head_ + 1) % kOutboxLen;
    outbox_count_--;
}

void Client::outbox_clear_() noexcept {
    outbox_head_ = 0;
    outbox_tail_ = 0;
    outbox_count_ = 0;
}

bool Client::subs_upsert_(const char* topic, int qos) noexcept {
    if (!topic) { return false; }
    const std::size_t tlen = std::strlen(topic);
    if (tlen == 0 || tlen >= kMaxTopicLen) { return false; }
    if (qos < 0) qos = 0;
    if (qos > 2) qos = 2;

    for (std::size_t i = 0; i < kSubMax; ++i) {
        if (subs_[i].used && subs_[i].topic_len == tlen &&
            std::memcmp(subs_[i].topic, topic, tlen) == 0) {
            subs_[i].qos = static_cast<std::int8_t>(qos);
            return true;
        }
    }

    for (std::size_t i = 0; i < kSubMax; ++i) {
        if (!subs_[i].used) {
            std::memcpy(subs_[i].topic, topic, tlen);
            subs_[i].topic[tlen] = '\0';
            subs_[i].topic_len = static_cast<std::uint16_t>(tlen);
            subs_[i].qos = static_cast<std::int8_t>(qos);
            subs_[i].used = true;
            return true;
        }
    }
    return false;
}

void Client::subs_clear_() noexcept {
    for (std::size_t i = 0; i < kSubMax; ++i) {
        subs_[i].used = false;
        subs_[i].topic[0] = '\0';
        subs_[i].topic_len = 0;
        subs_[i].qos = 0;
    }
}

esp_err_t Client::publish(const char* topic,
                          const void* payload,
                          std::size_t len,
                          int qos,
                          bool retain,
                          TickType_t /*timeout_ticks*/) noexcept {
    if (!eg_ || ((xEventGroupGetBits(eg_) & BIT_INITIALIZED) == 0)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!topic) {
        return ESP_ERR_INVALID_ARG;
    }

    lock(mutex_);
    const bool started = (xEventGroupGetBits(eg_) & BIT_STARTED) != 0;
    const bool connected = (xEventGroupGetBits(eg_) & BIT_CONNECTED) != 0;
    const bool ok = outbox_push_(topic, payload, len, qos, retain);
    unlock(mutex_);

    if (!started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!ok) {
        return (kOutboxLen == 0) ? ESP_ERR_INVALID_STATE : ESP_ERR_NO_MEM;
    }

    if (connected) {
        request_work_();
    }
    return ESP_OK;
}

esp_err_t Client::publish_str(const char* topic,
                             const char* payload,
                             int qos,
                             bool retain,
                             TickType_t timeout_ticks) noexcept {
    if (!payload) {
        return publish(topic, "", 0, qos, retain, timeout_ticks);
    }
    return publish(topic, payload, std::strlen(payload), qos, retain, timeout_ticks);
}

esp_err_t Client::subscribe(const char* topic, int qos, TickType_t /*timeout_ticks*/) noexcept {
    if (!eg_ || ((xEventGroupGetBits(eg_) & BIT_INITIALIZED) == 0)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!topic) {
        return ESP_ERR_INVALID_ARG;
    }

    lock(mutex_);
    const bool started = (xEventGroupGetBits(eg_) & BIT_STARTED) != 0;
    const bool ok = subs_upsert_(topic, qos);
    unlock(mutex_);

    if (!started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!ok) {
        return ESP_ERR_NO_MEM;
    }

    request_work_();
    return ESP_OK;
}

void Client::ctrl_task_entry(void* arg) noexcept {
    Client* self = static_cast<Client*>(arg);
    if (!self) {
        vTaskDelete(nullptr);
        return;
    }

    for (;;) {
        CtrlMsg msg{};
        if (xQueueReceive(self->ctrl_q_, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        const EventBits_t bits = xEventGroupGetBits(self->eg_);
        const bool started = (bits & BIT_STARTED) != 0;
        const bool connected = (bits & BIT_CONNECTED) != 0;

        if (!started) {
            (void)xTimerStop(self->retry_timer_, 0);
            continue;
        }

        if (msg.type == CtrlType::Disconnected) {
            lock(self->mutex_);
            const std::uint32_t delay_ms = compute_backoff_with_jitter(self->backoff_min_ms_,
                                                                       self->backoff_max_ms_,
                                                                       self->backoff_attempt_);
            self->backoff_attempt_++;
            unlock(self->mutex_);

            const TickType_t delay_ticks = pdMS_TO_TICKS(delay_ms);
            (void)xTimerStop(self->retry_timer_, 0);
            (void)xTimerChangePeriod(self->retry_timer_, (delay_ticks > 0 ? delay_ticks : 1), 0);
            (void)xTimerStart(self->retry_timer_, 0);

            ESP_LOGI(kTag, "Reconnect scheduled in %u ms", (unsigned)delay_ms);
            continue;
        }

        if (msg.type == CtrlType::RetryNow) {
            if (connected) {
                continue;
            }
            lock(self->mutex_);
            if (self->mqtt_client_) {
                esp_mqtt_client_handle_t h = static_cast<esp_mqtt_client_handle_t>(self->mqtt_client_);
                (void)esp_mqtt_client_reconnect(h);
            }
            unlock(self->mutex_);
            continue;
        }

        if (msg.type == CtrlType::Work) {
            if (!connected) {
                continue;
            }

            // Resubscribe cached topics
            lock(self->mutex_);
            esp_mqtt_client_handle_t h = static_cast<esp_mqtt_client_handle_t>(self->mqtt_client_);
            if (h) {
                for (std::size_t i = 0; i < kSubMax; ++i) {
                    if (self->subs_[i].used) {
                        (void)esp_mqtt_client_subscribe(h, self->subs_[i].topic, self->subs_[i].qos);
                    }
                }
            }
            unlock(self->mutex_);

            // Flush outbox (oldest-first)
            if (kOutboxLen > 0) {
                for (;;) {
                    OutboxItem out{};
                    lock(self->mutex_);
                    const bool has = self->outbox_peek_(out);
                    esp_mqtt_client_handle_t hh = static_cast<esp_mqtt_client_handle_t>(self->mqtt_client_);
                    unlock(self->mutex_);

                    if (!has || !hh) {
                        break;
                    }

                    const int msg_id = esp_mqtt_client_publish(hh,
                                                               out.topic,
                                                               reinterpret_cast<const char*>(out.payload),
                                                               (int)out.payload_len,
                                                               out.qos,
                                                               out.retain);
                    if (msg_id < 0) {
                        break; // keep message for next retry
                    }

                    lock(self->mutex_);
                    self->outbox_pop_();
                    unlock(self->mutex_);
                }
            }
        }
    }
}

void Client::rx_task_entry(void* arg) noexcept {
    Client* self = static_cast<Client*>(arg);
    if (!self) {
        vTaskDelete(nullptr);
        return;
    }

    for (;;) {
        RxItem item{};
        if (xQueueReceive(self->rx_q_, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        lock(self->mutex_);
        const MessageCallback cb = self->user_cb_;
        void* cb_ctx = self->user_cb_ctx_;
        unlock(self->mutex_);

        if (!cb) {
            continue;
        }

        MessageView view{};
        view.topic = item.topic;
        view.topic_len = item.topic_len;
        view.payload = item.payload;
        view.payload_len = item.payload_len;
        view.qos = item.qos;
        view.retain = (item.retain != 0);
        view.dup = (item.dup != 0);

        cb(cb_ctx, view);
    }
}

} // namespace mqtt_client
