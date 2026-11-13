#include "quality.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "config.h"
#include "esp_console.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "telemetry.h"

static const char *TAG = "quality";

namespace {

constexpr const char *NVS_NAMESPACE = "quality";

// Stored per position in NVS (key "cal<index>"): the chip the offsets were
// set for, and the offsets.
struct StoredCalibration {
    uint8_t type;
    uint32_t fingerprint;
    quality::Offsets offsets;
};

// Written by taskMeasure (bind) and the console, read by every task.
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// FR-1.6: samples rejected since start, per sensor position.
std::atomic<uint32_t> s_rejected[identity::SENSOR_COUNT] = {};
quality::Offsets s_offsets[identity::SENSOR_COUNT] = {};
identity::Sensor s_bound[identity::SENSOR_COUNT] = {};
bool s_is_bound[identity::SENSOR_COUNT] = {};

// The channels of the `cal` command, with their limits (main/config.h).
struct Channel {
    const char *name;
    const char *unit;
    float scale;  // stored units per unit of the channel
    float limit;
    int16_t quality::Offsets::*field;
};
constexpr Channel CHANNELS[] = {
    {"temperature", "\xC2\xB0" "C", 100.0f, config::CAL_MAX_TEMP_C, &quality::Offsets::temp_centi},
    {"humidity", "%RH", 100.0f, config::CAL_MAX_HUM_RH, &quality::Offsets::hum_centi},
    {"pressure", "hPa", 10.0f, config::CAL_MAX_PRESS_HPA, &quality::Offsets::press_deci},
};

void key_for(size_t index, char *key, size_t len)
{
    snprintf(key, len, "cal%u", static_cast<unsigned>(index));
}

// Saves the offsets of position `index` with the chip they belong to; all 0
// erases the key.
void store(size_t index, const identity::Sensor &s, const quality::Offsets &o)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "NVS unavailable, calibration kept until the next restart only");
        return;
    }
    char key[8];
    key_for(index, key, sizeof(key));
    esp_err_t err;
    if (o.any()) {
        const StoredCalibration stored = {s.type, s.fingerprint, o};
        err = nvs_set_blob(nvs, key, &stored, sizeof(stored));
    } else {
        err = nvs_erase_key(nvs, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Calibration not stored in NVS: %s", esp_err_to_name(err));
    }
}

bool same_chip(const identity::Sensor &a, uint8_t type, uint32_t fingerprint)
{
    return a.type == type && a.fingerprint == fingerprint;
}

const Channel *find_channel(const char *name)
{
    const size_t len = strlen(name);
    for (const Channel &ch : CHANNELS) {
        if (len > 0 && strncmp(ch.name, name, len) == 0) {  // "temp", "t" ... are accepted
            return &ch;
        }
    }
    return nullptr;
}

void print_calibration()
{
    for (size_t i = 0; i < identity::SENSOR_COUNT; i++) {
        char id[24], text[96];
        identity::sensor_id(i, id, sizeof(id));
        quality::format_offsets(quality::offsets(i), text, sizeof(text));
        printf("Sensor %u (%s): %s\n", static_cast<unsigned>(i), id[0] ? id : "unknown", text);
    }
}

// Sets the offsets of sensor 0 (the only position so far) and reports the change.
void change(const quality::Offsets &next)
{
    identity::Sensor s;
    if (!identity::sensor(0, s)) {
        printf("Sensor not known yet\n");
        return;
    }
    quality::Offsets previous;
    taskENTER_CRITICAL(&s_mux);
    previous = s_offsets[0];
    s_offsets[0] = next;
    taskEXIT_CRITICAL(&s_mux);
    if (previous == next) {
        printf("Calibration unchanged\n");
        return;
    }
    store(0, s, next);
    char before[96], after[96];
    quality::format_offsets(previous, before, sizeof(before));
    quality::format_offsets(next, after, sizeof(after));
    ESP_LOGI(TAG, "Calibration of sensor 0 changed: %s -> %s", before, after);
    // FR-10.4: the info message carries the offsets.
    telemetry::publish_info();  // fails harmlessly while not connected
}

// FR-11.1: `cal`, `cal set <channel> <offset>`, `cal clear [channel]`.
int cmd_cal(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "show") == 0) {
        print_calibration();
        return 0;
    }
    if (strcmp(argv[1], "set") == 0 && argc == 4) {
        const Channel *ch = find_channel(argv[2]);
        char *end = nullptr;
        const float value = strtof(argv[3], &end);
        if (ch == nullptr || end == argv[3] || *end != '\0' || !std::isfinite(value)) {
            printf("Usage: cal set temperature|humidity|pressure <offset>\n");
            return 1;
        }
        if (std::fabs(value) > ch->limit + 1e-4f) {
            printf("Offset out of range: %s limit is +/-%.2f %s\n", ch->name, ch->limit, ch->unit);
            return 1;
        }
        quality::Offsets next = quality::offsets(0);
        next.*(ch->field) = static_cast<int16_t>(lroundf(value * ch->scale));
        change(next);
        print_calibration();
        return 0;
    }
    if (strcmp(argv[1], "clear") == 0 && argc <= 3) {
        quality::Offsets next = quality::offsets(0);
        if (argc == 3) {
            const Channel *ch = find_channel(argv[2]);
            if (ch == nullptr) {
                printf("Usage: cal clear [temperature|humidity|pressure]\n");
                return 1;
            }
            next.*(ch->field) = 0;
        } else {
            next = {};
        }
        change(next);
        print_calibration();
        return 0;
    }
    printf("Usage: cal | cal set temperature|humidity|pressure <offset> | cal clear [channel]\n");
    return 1;
}

}  // namespace

namespace quality {

void bind_sensor(const identity::Sensor &s)
{
    if (s.index >= identity::SENSOR_COUNT) {
        return;
    }
    Offsets current;
    bool bound;
    identity::Sensor previous;
    taskENTER_CRITICAL(&s_mux);
    bound = s_is_bound[s.index];
    previous = s_bound[s.index];
    current = s_offsets[s.index];
    taskEXIT_CRITICAL(&s_mux);

    char id[24];
    identity::format_sensor_id(s.type, s.fingerprint, id, sizeof(id));

    if (bound) {
        // Re-initialization after I2C errors: same chip, nothing to do.
        if (same_chip(previous, s.type, s.fingerprint)) {
            return;
        }
        if (current.any()) {
            char text[96];
            format_offsets(current, text, sizeof(text));
            ESP_LOGW(TAG, "Sensor at position %u replaced, calibration discarded: %s", s.index, text);
        }
        current = {};
        store(s.index, s, current);
    } else {
        // Start-up: the offsets stored in NVS, if they belong to this chip.
        nvs_handle_t nvs;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
            char key[8];
            key_for(s.index, key, sizeof(key));
            StoredCalibration stored = {};
            size_t size = sizeof(stored);
            const bool found = nvs_get_blob(nvs, key, &stored, &size) == ESP_OK && size == sizeof(stored);
            nvs_close(nvs);
            if (found && same_chip(s, stored.type, stored.fingerprint)) {
                current = stored.offsets;
            } else if (found) {
                char text[96], old_id[24];
                format_offsets(stored.offsets, text, sizeof(text));
                identity::format_sensor_id(stored.type, stored.fingerprint, old_id, sizeof(old_id));
                ESP_LOGW(TAG, "Calibration of %s at position %u discarded, sensor is now %s: %s", old_id, s.index,
                         id, text);
                store(s.index, s, Offsets{});
            }
        }
    }

    taskENTER_CRITICAL(&s_mux);
    s_bound[s.index] = s;
    s_is_bound[s.index] = true;
    s_offsets[s.index] = current;
    taskEXIT_CRITICAL(&s_mux);

    if (current.any() && !bound) {
        char text[96];
        format_offsets(current, text, sizeof(text));
        ESP_LOGI(TAG, "Calibration of %s: %s", id, text);
    }
}

Offsets offsets(size_t index)
{
    if (index >= identity::SENSOR_COUNT) {
        return {};
    }
    taskENTER_CRITICAL(&s_mux);
    const Offsets o = s_offsets[index];
    taskEXIT_CRITICAL(&s_mux);
    return o;
}

void count_rejected(size_t index)
{
    if (index < identity::SENSOR_COUNT) {
        s_rejected[index]++;
    }
}

uint32_t rejected(size_t index)
{
    return index < identity::SENSOR_COUNT ? s_rejected[index].load() : 0;
}

void apply(size_t index, Bme280Data &d)
{
    const Offsets o = offsets(index);
    d.temperature_c += o.temp_centi / 100.0f;
    d.humidity_rh = std::clamp(d.humidity_rh + o.hum_centi / 100.0f, 0.0f, 100.0f);
    d.pressure_hpa += o.press_deci / 10.0f;
}

void format_offsets(const Offsets &o, char *buf, size_t len)
{
    if (!o.any()) {
        snprintf(buf, len, "not calibrated");
        return;
    }
    snprintf(buf, len, "temperature %+.2f \xC2\xB0" "C, humidity %+.2f %%RH, pressure %+.1f hPa",
             o.temp_centi / 100.0, o.hum_centi / 100.0, o.press_deci / 10.0);
}

esp_err_t register_console_command()
{
    esp_console_cmd_t cmd = {};
    cmd.command = "cal";
    cmd.help = "Sensor calibration: 'cal' shows the offsets, 'cal set <channel> <offset>' sets one "
               "(temperature in degC, humidity in %RH, pressure in hPa), 'cal clear [channel]' resets them";
    cmd.hint = "[set temperature|humidity|pressure <offset> | clear [channel]]";
    cmd.func = cmd_cal;
    return esp_console_cmd_register(&cmd);
}

int altitude_m() { return config::ALTITUDE_M; }

float msl_factor_at(float altitude_m)
{
    return std::pow(1.0f - 2.25577e-5f * altitude_m, -5.25588f);
}

float msl_factor()
{
    static const float factor = msl_factor_at(static_cast<float>(config::ALTITUDE_M));
    return factor;
}

bool dew_point(float temperature_c, float humidity_rh, float &out_c)
{
    constexpr float B = 17.62f;
    constexpr float C = 243.12f;  // degC
    if (temperature_c < -45.0f || temperature_c > 60.0f || !(humidity_rh > 0.0f)) {
        return false;
    }
    const float gamma = std::log(std::min(humidity_rh, 100.0f) / 100.0f) + B * temperature_c / (C + temperature_c);
    out_c = C * gamma / (B - gamma);
    return true;
}

}  // namespace quality
