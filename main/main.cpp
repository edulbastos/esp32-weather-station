// ESP32 Weather Station
// BME280 (temperature, humidity, pressure) shown on the on-board SSD1306 OLED.
//
// Tasks:
//   taskMeasure - reads the BME280 every MEASURE_PERIOD_MS
//   taskDisplay - redraws the OLED every DISPLAY_PERIOD_MS
// i2cMutex serializes access to the shared I2C bus, dataMutex protects the
// latest measurement shared between the two tasks.
// Wi-Fi and NTP (net_time) run in the background and provide the time of day.

#include <cstdio>
#include <ctime>
#include "bme280.h"
#include "config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net_time.h"
#include "ssd1306.h"

static const char *TAG = "weather";

namespace {

struct SharedData {
    Bme280Data values;
    bool valid;  // false until the first successful read or after sensor loss
};

i2c_master_bus_handle_t s_bus = nullptr;
Bme280 s_sensor;
Ssd1306 s_display;

SemaphoreHandle_t s_i2c_mutex = nullptr;   // i2cMutex
SemaphoreHandle_t s_data_mutex = nullptr;  // dataMutex
SharedData s_data = {};

constexpr TickType_t mutex_timeout() { return pdMS_TO_TICKS(config::MUTEX_TIMEOUT_MS); }

esp_err_t init_i2c_bus()
{
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = config::I2C_PORT;
    bus_cfg.sda_io_num = config::I2C_SDA_PIN;
    bus_cfg.scl_io_num = config::I2C_SCL_PIN;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = config::I2C_INTERNAL_PULLUP;
    return i2c_new_master_bus(&bus_cfg, &s_bus);
}

// Logs every device that answers on the bus (helps diagnosing wrong addresses).
void scan_i2c_bus()
{
    int found = 0;
    for (uint16_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(s_bus, addr, config::I2C_TIMEOUT_MS) == ESP_OK) {
            ESP_LOGI(TAG, "I2C device found at 0x%02X", addr);
            found++;
        }
    }
    if (found == 0) {
        ESP_LOGW(TAG, "No I2C devices found (SDA=%d, SCL=%d)", config::I2C_SDA_PIN, config::I2C_SCL_PIN);
    }
}

void init_status_led()
{
    gpio_config_t io_cfg = {};
    io_cfg.pin_bit_mask = 1ULL << config::STATUS_LED_PIN;
    io_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io_cfg);
    gpio_set_level(config::STATUS_LED_PIN, 0);
}

// FR-4.3: critical initialization error. Shows the message (if the display
// works), then stays here forever blinking the status LED.
[[noreturn]] void enter_error_mode(const char *message, bool display_ok)
{
    ESP_LOGE(TAG, "Critical error: %s. System halted.", message);
    if (display_ok) {
        s_display.clear();
        s_display.draw_string(0, 0, "ERROR", 2);
        s_display.draw_hline(0, 17, Ssd1306::WIDTH);
        s_display.draw_string(0, 24, message);
        s_display.draw_string(0, Ssd1306::HEIGHT - 8, "System halted");
        s_display.flush();
    }
    bool led = false;
    while (true) {
        led = !led;
        gpio_set_level(config::STATUS_LED_PIN, led);
        vTaskDelay(pdMS_TO_TICKS(config::ERROR_BLINK_PERIOD_MS));
    }
}

// Draws one "LABEL   value unit" row. The value is right-aligned so the
// decimal point stays in place when the number of digits changes.
void draw_value_row(int y, const char *label, const char *value, const char *unit, int scale)
{
    constexpr int UNIT_X = Ssd1306::WIDTH - 3 * 6;  // room for 3 small characters
    const int value_x = UNIT_X - 2 - Ssd1306::text_width(value, scale);
    const int small_y = y + (7 * scale - 7) / 2;      // vertically centered

    s_display.draw_string(0, small_y, label);
    s_display.draw_string(value_x, y, value, scale);
    s_display.draw_string(UNIT_X, small_y, unit);
}

// FR-5.7: Wi-Fi icon, 9x7 pixels. Each digit is the minimum signal level at
// which that pixel is drawn (0 = the dot, shown whenever connected).
constexpr int WIFI_ICON_W = 9;
constexpr int WIFI_ICON_H = 7;
constexpr const char *WIFI_ICON[WIFI_ICON_H] = {
    ".3333333.",
    "3.......3",
    "..22222..",
    ".2.....2.",
    "...111...",
    "..1...1..",
    "....0....",
};

// level: 0 = disconnected (drawn as an X), 1..3 = number of arcs.
void draw_wifi_icon(int x, int y, int level)
{
    for (int row = 0; row < WIFI_ICON_H; row++) {
        for (int col = 0; col < WIFI_ICON_W; col++) {
            const char c = WIFI_ICON[row][col];
            if (level == 0) {
                // X in the lower part of the icon, where the arcs would be
                const int dx = col - 4, dy = row - 3;
                if (row >= 1 && (dx == dy || dx == -dy)) {
                    s_display.draw_pixel(x + col, y + row);
                }
            } else if (c != '.' && c - '0' <= level) {
                s_display.draw_pixel(x + col, y + row);
            }
        }
    }
}

void render_screen(const SharedData &data)
{
    // 128x64: header in the top (yellow) band + three rows at scale 2 in the
    // remaining area. 128x32: three rows at scale 1, no header.
    constexpr bool TALL = Ssd1306::HEIGHT == 64;
    constexpr int SCALE = TALL ? 2 : 1;
    constexpr int HEADER_H = TALL ? (config::DISPLAY_YELLOW_ROWS > 0 ? config::DISPLAY_YELLOW_ROWS : 12) : 0;
    constexpr int ROW_PITCH = (Ssd1306::HEIGHT - HEADER_H) / 3;
    constexpr int ROW_H = 7 * SCALE;
    // Center each row inside its slot so no pixel crosses into the header band.
    constexpr int FIRST_ROW_Y = HEADER_H + (ROW_PITCH - ROW_H + 1) / 2;
    static_assert(ROW_PITCH >= ROW_H, "Display too small for the value rows");

    char temp[12], hum[12], press[12];
    if (data.valid) {
        snprintf(temp, sizeof(temp), "%.1f", data.values.temperature_c);
        snprintf(hum, sizeof(hum), "%.1f", data.values.humidity_rh);
        snprintf(press, sizeof(press), "%.1f", data.values.pressure_hpa);
    } else {
        snprintf(temp, sizeof(temp), "--.-");
        snprintf(hum, sizeof(hum), "--.-");
        snprintf(press, sizeof(press), "----.-");
    }

    // FR-5.3: the header shows the date and time once the clock is synchronized.
    char clock_text[20];
    struct tm now;
    const bool has_time = net_time::local_time(now);
    if (has_time) {
        strftime(clock_text, sizeof(clock_text), "%d/%m/%Y %H:%M", &now);
    }

    s_display.clear();
    if (TALL) {
        const char *title = !data.valid ? "SENSOR ERROR" : has_time ? clock_text : "WEATHER STATION";
        // Wi-Fi icon at the right edge; the title is centered in the space left of it.
        const int wifi = net_time::wifi_level();
        int title_area = Ssd1306::WIDTH;
        if (wifi >= 0) {
            draw_wifi_icon(Ssd1306::WIDTH - WIFI_ICON_W, (HEADER_H - WIFI_ICON_H) / 2, wifi);
            title_area -= WIFI_ICON_W + 2;
        }
        // Title vertically centered in the header band.
        s_display.draw_string((title_area - Ssd1306::text_width(title)) / 2, (HEADER_H - 7) / 2, title);
        if (config::DISPLAY_YELLOW_ROWS == 0) {
            s_display.draw_hline(0, HEADER_H - 2, Ssd1306::WIDTH);  // two-color panels need no separator
        }
    }
    draw_value_row(FIRST_ROW_Y + 0 * ROW_PITCH, "TEMP", temp, "\x7F" "C", SCALE);
    draw_value_row(FIRST_ROW_Y + 1 * ROW_PITCH, "HUM", hum, "%RH", SCALE);
    draw_value_row(FIRST_ROW_Y + 2 * ROW_PITCH, "PRES", press, "hPa", SCALE);
}

void taskMeasure(void *)
{
    int errors = 0;
    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        Bme280Data sample = {};
        esp_err_t err = ESP_ERR_TIMEOUT;

        if (xSemaphoreTake(s_i2c_mutex, mutex_timeout()) == pdTRUE) {
            err = s_sensor.read(sample);
            // NFR-2.2: try to recover the sensor after repeated failures.
            if (err != ESP_OK && errors + 1 >= config::MAX_CONSECUTIVE_ERRORS) {
                ESP_LOGW(TAG, "Re-initializing BME280");
                if (s_sensor.init(s_bus, config::BME280_ADDR, config::I2C_FREQ_HZ) == ESP_OK) {
                    err = s_sensor.read(sample);
                }
            }
            xSemaphoreGive(s_i2c_mutex);
        } else {
            ESP_LOGW(TAG, "taskMeasure: I2C bus busy");
        }

        if (err == ESP_OK) {
            errors = 0;
            // FR-5.4: timestamp the reading once the clock is synchronized.
            char stamp[24] = "";
            struct tm now;
            if (net_time::local_time(now)) {
                strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S  ", &now);
            }
            ESP_LOGI(TAG, "%sT=%.2f C  H=%.2f %%RH  P=%.2f hPa  Alt~%.0f m", stamp,
                     sample.temperature_c, sample.humidity_rh, sample.pressure_hpa,
                     Bme280::altitude_m(sample.pressure_hpa, config::SEA_LEVEL_PRESSURE_HPA));
        } else {
            errors++;
            ESP_LOGW(TAG, "BME280 read failed (%d in a row): %s", errors, esp_err_to_name(err));
        }

        if (xSemaphoreTake(s_data_mutex, mutex_timeout()) == pdTRUE) {
            if (err == ESP_OK) {
                s_data.values = sample;
                s_data.valid = true;
            } else if (errors >= config::MAX_CONSECUTIVE_ERRORS) {
                s_data.valid = false;  // stop showing stale values
            }
            xSemaphoreGive(s_data_mutex);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(config::MEASURE_PERIOD_MS));
    }
}

void taskDisplay(void *)
{
    int errors = 0;
    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        SharedData snapshot = {};
        if (xSemaphoreTake(s_data_mutex, mutex_timeout()) == pdTRUE) {
            snapshot = s_data;
            xSemaphoreGive(s_data_mutex);
        }

        // Rendering only touches the local frame buffer, no bus access needed.
        render_screen(snapshot);

        esp_err_t err = ESP_ERR_TIMEOUT;
        if (xSemaphoreTake(s_i2c_mutex, mutex_timeout()) == pdTRUE) {
            err = s_display.flush();
            // NFR-2.2: try to recover the display after repeated failures.
            if (err != ESP_OK && errors + 1 >= config::MAX_CONSECUTIVE_ERRORS) {
                ESP_LOGW(TAG, "Re-initializing SSD1306");
                if (s_display.init(s_bus, config::SSD1306_ADDR, config::I2C_FREQ_HZ) == ESP_OK) {
                    render_screen(snapshot);  // init() cleared the frame buffer
                    err = s_display.flush();
                }
            }
            xSemaphoreGive(s_i2c_mutex);
        } else {
            ESP_LOGW(TAG, "taskDisplay: I2C bus busy");
        }

        if (err == ESP_OK) {
            errors = 0;
        } else {
            errors++;
            ESP_LOGW(TAG, "Display update failed (%d in a row): %s", errors, esp_err_to_name(err));
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(config::DISPLAY_PERIOD_MS));
    }
}

}  // namespace

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 Weather Station starting");
    init_status_led();

    esp_err_t err = init_i2c_bus();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus initialization failed: %s", esp_err_to_name(err));
        enter_error_mode("I2C bus failure", false);
    }
    scan_i2c_bus();

    // FR-4.1
    err = s_display.init(s_bus, config::SSD1306_ADDR, config::I2C_FREQ_HZ);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SSD1306 display initialization failed at 0x%02X: %s",
                 config::SSD1306_ADDR, esp_err_to_name(err));
        enter_error_mode("Display not found", false);
    }

    s_display.clear();
    s_display.draw_string(0, 0, "Weather Station");
    s_display.draw_string(0, 16, "Starting...");
    s_display.flush();

    // FR-4.2
    err = s_sensor.init(s_bus, config::BME280_ADDR, config::I2C_FREQ_HZ);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BME280 sensor initialization failed at 0x%02X: %s",
                 config::BME280_ADDR, esp_err_to_name(err));
        enter_error_mode("BME280 not found", true);
    }

    s_i2c_mutex = xSemaphoreCreateMutex();
    s_data_mutex = xSemaphoreCreateMutex();
    if (s_i2c_mutex == nullptr || s_data_mutex == nullptr) {
        enter_error_mode("Out of memory", true);
    }

    if (xTaskCreate(taskMeasure, "taskMeasure", config::MEASURE_TASK_STACK, nullptr,
                    config::MEASURE_TASK_PRIO, nullptr) != pdPASS ||
        xTaskCreate(taskDisplay, "taskDisplay", config::DISPLAY_TASK_STACK, nullptr,
                    config::DISPLAY_TASK_PRIO, nullptr) != pdPASS) {
        enter_error_mode("Task creation failed", true);
    }

    // FR-5.5: Wi-Fi/NTP problems are not critical, the station keeps running.
    err = net_time::start();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Time of day unavailable: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Running: measure every %lu ms, display every %lu ms",
             static_cast<unsigned long>(config::MEASURE_PERIOD_MS),
             static_cast<unsigned long>(config::DISPLAY_PERIOD_MS));
}
