#include "telemetry.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include "config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "mqtt_client.h"

static const char *TAG = "telemetry";

namespace {

// FR-6.5: the client does not reconnect on its own (disable_auto_reconnect).
// This module retries every MQTT_RECONNECT_MS while Wi-Fi has an IP address,
// does not try at all while Wi-Fi is down, and reconnects at once when the
// IP address comes back.
esp_mqtt_client_handle_t s_client = nullptr;
char s_client_id[40] = "";
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
        ESP_LOGI(TAG, "Connected to MQTT broker %s as \"%s\"", config::MQTT_BROKER_URI, s_client_id);
        s_connected = true;
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

    // Fixed, readable client ID, unique per board (from the MAC address).
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_client_id, sizeof(s_client_id), "%s%02x%02x%02x", config::MQTT_CLIENT_ID_PREFIX, mac[3], mac[4],
             mac[5]);
    cfg.credentials.client_id = s_client_id;

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
                 config::MQTT_TOPIC, static_cast<unsigned long>(config::MQTT_PUBLISH_PERIOD_MS));
    }
    return err;
}

bool enabled() { return s_client != nullptr; }

bool connected() { return s_connected; }

const char *client_id() { return s_client_id; }

esp_err_t publish(const Bme280Data &data, time_t sample_time)
{
    if (!s_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    // FR-6.3: JSON payload, the time is left out until the clock is synchronized.
    char time_field[32] = "";
    if (sample_time != 0) {
        snprintf(time_field, sizeof(time_field), "\"time\":%" PRId64 ",", static_cast<int64_t>(sample_time));
    }
    char payload[160];
    const int len = snprintf(payload, sizeof(payload),
                             "{%s\"temperature_c\":%.2f,\"humidity_rh\":%.2f,\"pressure_hpa\":%.2f}",
                             time_field, data.temperature_c, data.humidity_rh, data.pressure_hpa);

    const int id = esp_mqtt_client_publish(s_client, config::MQTT_TOPIC, payload, len,
                                           config::MQTT_QOS, config::MQTT_RETAIN);
    if (id < 0) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Published %s", payload);
    return ESP_OK;
}

}  // namespace telemetry
