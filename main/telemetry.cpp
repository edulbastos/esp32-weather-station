#include "telemetry.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include "config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "mqtt_client.h"

static const char *TAG = "telemetry";

namespace {

esp_mqtt_client_handle_t s_client = nullptr;
std::atomic<bool> s_client_started{false};
std::atomic<bool> s_connected{false};

void on_mqtt_event(void *, esp_event_base_t, int32_t id, void *)
{
    switch (static_cast<esp_mqtt_event_id_t>(id)) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to MQTT broker %s", config::MQTT_BROKER_URI);
        s_connected = true;
        break;
    case MQTT_EVENT_DISCONNECTED:
        // FR-6.5: the client reconnects on its own.
        if (s_connected.exchange(false)) {
            ESP_LOGW(TAG, "Disconnected from MQTT broker, retrying in %lu ms",
                     static_cast<unsigned long>(config::MQTT_RECONNECT_MS));
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT connection error");
        break;
    default:
        break;
    }
}

// The broker can only be reached once Wi-Fi has an IP address, so the client
// is started on the first IP_EVENT_STA_GOT_IP instead of at boot.
void on_got_ip(void *, esp_event_base_t, int32_t, void *)
{
    if (!s_client_started.exchange(true)) {
        const esp_err_t err = esp_mqtt_client_start(s_client);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "MQTT client start failed: %s", esp_err_to_name(err));
            s_client_started = false;
        }
    }
}

}  // namespace

namespace telemetry {

esp_err_t start()
{
    if (config::MQTT_BROKER_URI[0] == '\0') {
        ESP_LOGW(TAG, "MQTT broker not configured (idf.py menuconfig), data transmission disabled");
        return ESP_ERR_INVALID_STATE;
    }

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = config::MQTT_BROKER_URI;
    if (config::MQTT_USERNAME[0] != '\0') {
        cfg.credentials.username = config::MQTT_USERNAME;
        cfg.credentials.authentication.password = config::MQTT_PASSWORD;
    }
    cfg.network.reconnect_timeout_ms = config::MQTT_RECONNECT_MS;

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == nullptr) {
        return ESP_FAIL;
    }
    esp_err_t err = esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, on_mqtt_event, nullptr);
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, nullptr);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Publishing to %s, topic \"%s\", every %lu ms", config::MQTT_BROKER_URI,
                 config::MQTT_TOPIC, static_cast<unsigned long>(config::MQTT_PUBLISH_PERIOD_MS));
    }
    return err;
}

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
