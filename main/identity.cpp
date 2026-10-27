#include "identity.h"

#include <cstdio>
#include <cstring>
#include "config.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "identity";

namespace {

uint8_t s_mac[6] = {};
char s_device_id[20] = "";
char s_name[config::DEVICE_NAME_MAX + 1] = "";
char s_name_json[config::DEVICE_NAME_MAX * 6 + 1] = "";  // worst case: every character as \u00XX
bool s_nvs_ok = false;

// Written by taskMeasure, read by the MQTT, web and log tasks.
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
identity::Sensor s_sensors[identity::SENSOR_COUNT] = {};
bool s_known[identity::SENSOR_COUNT] = {};

constexpr const char *NVS_NAMESPACE = "identity";

// Stored per position in NVS (key "sensor<index>").
struct StoredSensor {
    uint8_t type;
    uint8_t address;
    uint32_t fingerprint;
};

void json_escape(const char *in, char *out, size_t len)
{
    size_t o = 0;
    for (; *in != '\0' && o + 7 < len; in++) {
        const auto c = static_cast<unsigned char>(*in);
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = static_cast<char>(c);
        } else if (c < 0x20) {
            o += snprintf(out + o, len - o, "\\u%04x", c);
        } else {
            out[o++] = static_cast<char>(c);
        }
    }
    out[o] = '\0';
}

// Channels of a BME280 (FR-10.3), with the resolution of the history (FR-10.6).
struct Channel {
    const char *id;
    const char *unit;
    const char *resolution;
};
constexpr Channel BME280_CHANNELS[] = {
    {"temperature", "°C", "0.01"},
    {"humidity", "%RH", "0.01"},
    {"pressure", "hPa", "0.1"},
};

}  // namespace

namespace identity {

esp_err_t init()
{
    // FR-10.1: the base MAC address (the Wi-Fi station MAC) from eFuse.
    esp_err_t err = esp_read_mac(s_mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        return err;
    }
    format_device_id(s_mac, s_device_id, sizeof(s_device_id));

    // FR-10.2: the name is for people only; longer names are cut.
    if (strlen(config::DEVICE_NAME) > config::DEVICE_NAME_MAX) {
        ESP_LOGW(TAG, "Device name longer than %u characters, cut", static_cast<unsigned>(config::DEVICE_NAME_MAX));
    }
    snprintf(s_name, sizeof(s_name), "%s", config::DEVICE_NAME);
    json_escape(s_name, s_name_json, sizeof(s_name_json));
    ESP_LOGI(TAG, "Device %s%s%s%s, firmware %s", s_device_id, s_name[0] ? " (\"" : "", s_name,
             s_name[0] ? "\")" : "", firmware_version());

    // FR-10.7: the last sensor per position is kept in NVS. Without NVS the
    // station runs on, but cannot tell a replacement across restarts.
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    s_nvs_ok = err == ESP_OK;
    if (!s_nvs_ok) {
        ESP_LOGW(TAG, "NVS unavailable, sensor replacements across restarts not detected: %s",
                 esp_err_to_name(err));
    }
    return ESP_OK;
}

const uint8_t *device_mac() { return s_mac; }

const char *device_id() { return s_device_id; }

const char *device_name() { return s_name; }

const char *device_name_json() { return s_name_json; }

const char *firmware_version() { return esp_app_get_description()->version; }

void format_device_id(const uint8_t mac[6], char *buf, size_t len)
{
    snprintf(buf, len, "ws-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

const char *sensor_type_name(uint8_t type)
{
    return type == SENSOR_BME280 ? "bme280" : "unknown";
}

void format_sensor_id(uint8_t type, uint32_t fingerprint, char *buf, size_t len)
{
    snprintf(buf, len, "%s-%08lx", sensor_type_name(type), static_cast<unsigned long>(fingerprint));
}

bool set_sensor(const Sensor &s)
{
    if (s.index >= SENSOR_COUNT) {
        return false;
    }
    Sensor previous = {};
    bool known;
    taskENTER_CRITICAL(&s_mux);
    known = s_known[s.index];
    previous = s_sensors[s.index];
    s_sensors[s.index] = s;
    s_known[s.index] = true;
    taskEXIT_CRITICAL(&s_mux);

    // Across restarts, the previous sensor comes from NVS.
    nvs_handle_t nvs = 0;
    const bool nvs_open_ok = s_nvs_ok && nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK;
    char key[12];
    snprintf(key, sizeof(key), "sensor%u", s.index);
    if (!known && nvs_open_ok) {
        StoredSensor stored = {};
        size_t size = sizeof(stored);
        if (nvs_get_blob(nvs, key, &stored, &size) == ESP_OK && size == sizeof(stored)) {
            known = true;
            previous = Sensor{stored.type, s.index, stored.address, stored.fingerprint};
        }
    }

    const bool replaced = known && (previous.type != s.type || previous.fingerprint != s.fingerprint);
    char id[24];
    format_sensor_id(s.type, s.fingerprint, id, sizeof(id));
    if (replaced) {
        char old_id[24];
        format_sensor_id(previous.type, previous.fingerprint, old_id, sizeof(old_id));
        ESP_LOGW(TAG, "Sensor at position %u (i2c@0x%02x) replaced: %s -> %s", s.index, s.address, old_id, id);
    } else if (!known) {
        ESP_LOGI(TAG, "Sensor %s at position %u (i2c@0x%02x), first seen", id, s.index, s.address);
    }

    if (nvs_open_ok) {
        if (replaced || !known || previous.address != s.address) {
            const StoredSensor stored = {s.type, s.address, s.fingerprint};
            if (nvs_set_blob(nvs, key, &stored, sizeof(stored)) != ESP_OK || nvs_commit(nvs) != ESP_OK) {
                ESP_LOGW(TAG, "Sensor %s not stored in NVS", id);
            }
        }
        nvs_close(nvs);
    }
    return replaced;
}

bool sensor(size_t index, Sensor &out)
{
    if (index >= SENSOR_COUNT) {
        return false;
    }
    taskENTER_CRITICAL(&s_mux);
    const bool known = s_known[index];
    out = s_sensors[index];
    taskEXIT_CRITICAL(&s_mux);
    return known;
}

void sensor_id(size_t index, char *buf, size_t len)
{
    Sensor s;
    if (sensor(index, s)) {
        format_sensor_id(s.type, s.fingerprint, buf, len);
    } else if (len > 0) {
        buf[0] = '\0';
    }
}

int info_json(char *buf, size_t len)
{
    int n = snprintf(buf, len,
                     "{\"device_id\":\"%s\",\"name\":\"%s\",\"firmware\":\"%s\",\"measure_period_s\":%lu,"
                     "\"publish_period_s\":%lu,\"log_period_s\":%lu,\"sensors\":[",
                     s_device_id, s_name_json, firmware_version(),
                     static_cast<unsigned long>(config::MEASURE_PERIOD_MS / 1000),
                     static_cast<unsigned long>(config::MQTT_PUBLISH_PERIOD_MS / 1000),
                     static_cast<unsigned long>(config::LOG_PERIOD_MS / 1000));
    bool first = true;
    for (size_t i = 0; i < SENSOR_COUNT && n >= 0 && static_cast<size_t>(n) < len; i++) {
        Sensor s;
        if (!sensor(i, s)) {
            continue;
        }
        char id[24];
        format_sensor_id(s.type, s.fingerprint, id, sizeof(id));
        n += snprintf(buf + n, len - n,
                      "%s{\"id\":\"%s\",\"type\":\"%s\",\"index\":%u,\"address\":\"i2c@0x%02x\",\"channels\":[",
                      first ? "" : ",", id, sensor_type_name(s.type), s.index, s.address);
        first = false;
        for (size_t c = 0; c < sizeof(BME280_CHANNELS) / sizeof(BME280_CHANNELS[0]); c++) {
            if (n < 0 || static_cast<size_t>(n) >= len) {
                return n;
            }
            const Channel &ch = BME280_CHANNELS[c];
            n += snprintf(buf + n, len - n, "%s{\"id\":\"%s\",\"unit\":\"%s\",\"resolution\":%s}", c ? "," : "",
                          ch.id, ch.unit, ch.resolution);
        }
        if (n >= 0 && static_cast<size_t>(n) < len) {
            n += snprintf(buf + n, len - n, "]}");
        }
    }
    if (n >= 0 && static_cast<size_t>(n) < len) {
        n += snprintf(buf + n, len - n, "]}");
    }
    return n;
}

}  // namespace identity
