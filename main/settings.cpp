#include "settings.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "settings";

namespace {

constexpr const char *NVS_NAMESPACE = "settings";
constexpr const char *NVS_KEY = "set";
constexpr uint8_t FORMAT = 1;

// Stored in NVS as one blob; a blob of another size or format is ignored.
struct Stored {
    uint8_t format;
    settings::Settings values;
};

settings::Settings s_active = {};
bool s_stored = false;

void copy(char *dst, size_t len, const char *src)
{
    snprintf(dst, len, "%s", src);
}

const char *masked(const char *password)
{
    return password[0] ? "***" : "(none)";
}

}  // namespace

namespace settings {

Settings defaults()
{
    Settings s = {};
    copy(s.wifi_ssid, sizeof(s.wifi_ssid), config::WIFI_SSID);
    copy(s.wifi_password, sizeof(s.wifi_password), config::WIFI_PASSWORD);
    copy(s.mqtt_uri, sizeof(s.mqtt_uri), config::MQTT_BROKER_URI);
    copy(s.mqtt_username, sizeof(s.mqtt_username), config::MQTT_USERNAME);
    copy(s.mqtt_password, sizeof(s.mqtt_password), config::MQTT_PASSWORD);
    // FR-10.2: longer names are cut.
    if (strlen(config::DEVICE_NAME) > config::DEVICE_NAME_MAX) {
        ESP_LOGW(TAG, "Device name longer than %u characters, cut", static_cast<unsigned>(config::DEVICE_NAME_MAX));
    }
    copy(s.name, sizeof(s.name), config::DEVICE_NAME);
    s.altitude_m = config::ALTITUDE_M;
    copy(s.metar, sizeof(s.metar), config::METAR_STATION);
    for (char *c = s.metar; *c; c++) {
        *c = static_cast<char>(toupper(static_cast<unsigned char>(*c)));
    }
    return s;
}

void init()
{
    s_active = defaults();
    s_stored = false;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        Stored stored = {};
        size_t size = sizeof(stored);
        const esp_err_t err = nvs_get_blob(nvs, NVS_KEY, &stored, &size);
        if (err == ESP_OK && size == sizeof(stored) && stored.format == FORMAT) {
            // Terminate every string, whatever the flash holds.
            Settings &v = stored.values;
            v.wifi_ssid[sizeof(v.wifi_ssid) - 1] = '\0';
            v.wifi_password[sizeof(v.wifi_password) - 1] = '\0';
            v.mqtt_uri[sizeof(v.mqtt_uri) - 1] = '\0';
            v.mqtt_username[sizeof(v.mqtt_username) - 1] = '\0';
            v.mqtt_password[sizeof(v.mqtt_password) - 1] = '\0';
            v.name[sizeof(v.name) - 1] = '\0';
            v.metar[sizeof(v.metar) - 1] = '\0';
            s_active = v;
            s_stored = true;
        } else if (err == ESP_OK) {
            ESP_LOGW(TAG, "Stored settings of another format ignored, using menuconfig");
        }
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "Settings: %s", source_name());
}

const Settings &get() { return s_active; }

const char *source_name() { return s_stored ? "stored" : "menuconfig"; }

bool stored() { return s_stored; }

bool validate(Settings &s, char *why, size_t len)
{
    const size_t ssid = strlen(s.wifi_ssid);
    const size_t pass = strlen(s.wifi_password);
    const size_t uri = strlen(s.mqtt_uri);
    for (char *c = s.metar; *c; c++) {
        *c = static_cast<char>(toupper(static_cast<unsigned char>(*c)));
    }
    if (ssid == 0 || ssid > config::SETTINGS_SSID_MAX) {
        snprintf(why, len, "Wi-Fi network: 1 to %u bytes", static_cast<unsigned>(config::SETTINGS_SSID_MAX));
    } else if (pass != 0 && (pass < config::SETTINGS_WIFI_PASS_MIN || pass > config::SETTINGS_WIFI_PASS_MAX)) {
        snprintf(why, len, "Wi-Fi password: empty or %u to %u characters",
                 static_cast<unsigned>(config::SETTINGS_WIFI_PASS_MIN),
                 static_cast<unsigned>(config::SETTINGS_WIFI_PASS_MAX));
    } else if (uri > config::SETTINGS_MQTT_URI_MAX ||
               (uri != 0 && strncmp(s.mqtt_uri, "mqtt://", 7) != 0 && strncmp(s.mqtt_uri, "mqtts://", 8) != 0)) {
        snprintf(why, len, "MQTT broker: empty, or mqtt://... or mqtts://..., up to %u characters",
                 static_cast<unsigned>(config::SETTINGS_MQTT_URI_MAX));
    } else if (strlen(s.mqtt_username) > config::SETTINGS_MQTT_USER_MAX ||
               strlen(s.mqtt_password) > config::SETTINGS_MQTT_PASS_MAX) {
        snprintf(why, len, "MQTT user name and password: up to %u characters",
                 static_cast<unsigned>(config::SETTINGS_MQTT_USER_MAX));
    } else if (strlen(s.name) > config::DEVICE_NAME_MAX) {
        snprintf(why, len, "Name: up to %u characters", static_cast<unsigned>(config::DEVICE_NAME_MAX));
    } else if (s.altitude_m < config::SETTINGS_ALTITUDE_MIN_M || s.altitude_m > config::SETTINGS_ALTITUDE_MAX_M) {
        snprintf(why, len, "Altitude: %d to %d m", config::SETTINGS_ALTITUDE_MIN_M, config::SETTINGS_ALTITUDE_MAX_M);
    } else if (s.metar[0] != '\0' &&
               (strlen(s.metar) != 4 || !isalpha(static_cast<unsigned char>(s.metar[0])) ||
                !isalpha(static_cast<unsigned char>(s.metar[1])) || !isalpha(static_cast<unsigned char>(s.metar[2])) ||
                !isalpha(static_cast<unsigned char>(s.metar[3])))) {
        snprintf(why, len, "METAR airport: empty or 4 letters (ICAO code)");
    } else {
        return true;
    }
    return false;
}

esp_err_t save(const Settings &s)
{
    Stored stored = {};
    stored.format = FORMAT;
    stored.values = s;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(nvs, NVS_KEY, &stored, sizeof(stored));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        // The passwords are never logged (FR-12.4).
        ESP_LOGI(TAG, "Settings stored: Wi-Fi \"%s\", MQTT broker %s, name \"%s\", altitude %ld m, METAR %s",
                 s.wifi_ssid, s.mqtt_uri[0] ? s.mqtt_uri : "(off)", s.name, static_cast<long>(s.altitude_m),
                 s.metar[0] ? s.metar : "(off)");
    }
    return err;
}

esp_err_t clear()
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(nvs, NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;  // nothing stored
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Stored settings erased, menuconfig values apply after the restart");
    }
    return err;
}

void print()
{
    const Settings &s = s_active;
    printf("Settings (%s):\n", source_name());
    printf("  Wi-Fi network: \"%s\", password %s\n", s.wifi_ssid, masked(s.wifi_password));
    if (s.mqtt_uri[0]) {
        printf("  MQTT broker: %s, user \"%s\", password %s\n", s.mqtt_uri, s.mqtt_username, masked(s.mqtt_password));
    } else {
        printf("  MQTT broker: (off)\n");
    }
    printf("  Name: \"%s\", altitude %ld m, METAR airport %s\n", s.name, static_cast<long>(s.altitude_m),
           s.metar[0] ? s.metar : "(off)");
}

}  // namespace settings
