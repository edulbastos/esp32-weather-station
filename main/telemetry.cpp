#include "telemetry.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include "config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "identity.h"
#include "mqtt_client.h"

static const char *TAG = "telemetry";

namespace {

// FR-6.5: the client does not reconnect on its own (disable_auto_reconnect).
// This module retries every MQTT_RECONNECT_MS while Wi-Fi has an IP address,
// does not try at all while Wi-Fi is down, and reconnects at once when the
// IP address comes back.
esp_mqtt_client_handle_t s_client = nullptr;
char s_data_topic[48] = "";
char s_info_topic[48] = "";
TimerHandle_t s_retry_timer = nullptr;
std::atomic<bool> s_client_started{false};
std::atomic<bool> s_connected{false};
std::atomic<bool> s_has_ip{false};

void reconnect_now()
{
    // Only acts while the client waits for a reconnection; otherwise the next
    // MQTT_EVENT_DISCONNECTED schedules the retry.
    esp_mqtt_client_reconnect(s_client);
}

void on_retry_timer(TimerHandle_t)
{
    if (s_has_ip) {
        reconnect_now();
    }
}

void on_mqtt_event(void *, esp_event_base_t, int32_t id, void *)
{
    switch (static_cast<esp_mqtt_event_id_t>(id)) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to MQTT broker %s as \"%s\"", config::MQTT_BROKER_URI, identity::device_id());
        s_connected = true;
        telemetry::publish_info();
        // FR-10.4: subscribers of the topic used before v2.0.0 no longer get
        // a stale reading (empty retained message = delete).
        esp_mqtt_client_publish(s_client, config::MQTT_LEGACY_TOPIC, "", 0, config::MQTT_QOS, true);
        break;
    case MQTT_EVENT_DISCONNECTED: {
        // Also sent after every failed connection attempt.
        const bool was_connected = s_connected.exchange(false);
        if (s_has_ip) {
            xTimerReset(s_retry_timer, 0);
            if (was_connected) {
                ESP_LOGW(TAG, "Disconnected from MQTT broker, retrying in %lu ms",
                         static_cast<unsigned long>(config::MQTT_RECONNECT_MS));
            }
        } else if (was_connected) {
            ESP_LOGW(TAG, "Disconnected from MQTT broker, waiting for Wi-Fi");
        }
        break;
    }
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT connection error");
        break;
    default:
        break;
    }
}

// The broker can only be reached once Wi-Fi has an IP address, so the client
// is started on the first IP_EVENT_STA_GOT_IP instead of at boot; on later
// ones it reconnects without waiting for the retry timer.
void on_got_ip(void *, esp_event_base_t, int32_t, void *)
{
    s_has_ip = true;
    if (!s_client_started.exchange(true)) {
        const esp_err_t err = esp_mqtt_client_start(s_client);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "MQTT client start failed: %s", esp_err_to_name(err));
            s_client_started = false;
        }
    } else {
        xTimerStop(s_retry_timer, 0);
        reconnect_now();
    }
}

void on_wifi_lost(void *, esp_event_base_t, int32_t, void *)
{
    s_has_ip = false;
    xTimerStop(s_retry_timer, 0);
}

}  // namespace

namespace telemetry {

esp_err_t start()
{
    if (config::MQTT_BROKER_URI[0] == '\0') {
        ESP_LOGW(TAG, "MQTT broker not configured (idf.py menuconfig), data transmission disabled");
        return ESP_ERR_INVALID_STATE;
    }

    s_retry_timer = xTimerCreate("mqttRetry", pdMS_TO_TICKS(config::MQTT_RECONNECT_MS), pdFALSE,
                                 nullptr, on_retry_timer);
    if (s_retry_timer == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = config::MQTT_BROKER_URI;
    if (config::MQTT_USERNAME[0] != '\0') {
        cfg.credentials.username = config::MQTT_USERNAME;
        cfg.credentials.authentication.password = config::MQTT_PASSWORD;
    }
    cfg.network.disable_auto_reconnect = true;
    // FR-6.5: a connection that stops delivering is noticed after about
    // 1.5 keepalive periods (ping after half a period, then a full period);
    // shorter stalls are ridden out.
    cfg.session.keepalive = config::MQTT_KEEPALIVE_S;
    // FR-6.5: a short stall of the link (seen at about -65 dBm) blocks a send
    // for seconds; the default 10 s then dropped the connection.
    cfg.network.timeout_ms = config::MQTT_NETWORK_TIMEOUT_MS;
    // FR-6.4: QoS 1 messages stay in the outbox until the broker confirms
    // them and are resent after a reconnection; bound the memory they use.
    cfg.outbox.limit = config::MQTT_OUTBOX_LIMIT;

    // FR-10.1: the client ID is the device identifier.
    cfg.credentials.client_id = identity::device_id();
    snprintf(s_data_topic, sizeof(s_data_topic), "%s/%s/data", config::MQTT_TOPIC_PREFIX, identity::device_id());
    snprintf(s_info_topic, sizeof(s_info_topic), "%s/%s/info", config::MQTT_TOPIC_PREFIX, identity::device_id());

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == nullptr) {
        return ESP_FAIL;
    }
    esp_err_t err = esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, on_mqtt_event, nullptr);
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, nullptr);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi_lost, nullptr);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_wifi_lost, nullptr);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Publishing to %s, topic \"%s\", every %lu ms", config::MQTT_BROKER_URI,
                 s_data_topic, static_cast<unsigned long>(config::MQTT_PUBLISH_PERIOD_MS));
    }
    return err;
}

bool enabled() { return s_client != nullptr; }

bool connected() { return s_connected; }

const char *client_id() { return identity::device_id(); }

esp_err_t publish(const Bme280Data &data, time_t sample_time)
{
    if (!s_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    // FR-6.3, FR-10.4: JSON payload with the device and sensor identifiers;
    // the time is left out until the clock is synchronized.
    char time_field[32] = "";
    if (sample_time != 0) {
        snprintf(time_field, sizeof(time_field), "\"time\":%" PRId64 ",", static_cast<int64_t>(sample_time));
    }
    char sensor_id[24];
    identity::sensor_id(0, sensor_id, sizeof(sensor_id));
    char payload[256];
    const int len = snprintf(payload, sizeof(payload),
                             "{\"device_id\":\"%s\",\"sensor_id\":\"%s\",%s\"temperature_c\":%.2f,"
                             "\"humidity_rh\":%.2f,\"pressure_hpa\":%.2f}",
                             identity::device_id(), sensor_id, time_field, data.temperature_c, data.humidity_rh,
                             data.pressure_hpa);

    const int id = esp_mqtt_client_publish(s_client, s_data_topic, payload, len,
                                           config::MQTT_QOS, config::MQTT_RETAIN);
    if (id < 0) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Published %s", payload);
    return ESP_OK;
}

esp_err_t publish_info()
{
    if (!s_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    char payload[768];
    const int len = identity::info_json(payload, sizeof(payload));
    if (len < 0 || static_cast<size_t>(len) >= sizeof(payload)) {
        ESP_LOGE(TAG, "Info message does not fit %u bytes", static_cast<unsigned>(sizeof(payload)));
        return ESP_ERR_INVALID_SIZE;
    }
    if (esp_mqtt_client_publish(s_client, s_info_topic, payload, len, config::MQTT_QOS, true) < 0) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Published info to %s", s_info_topic);
    return ESP_OK;
}

}  // namespace telemetry
