// ESP32 Weather Station
// BME280 (temperature, humidity, pressure) shown on the on-board SSD1306 OLED.
//
// Tasks:
//   taskMeasure - reads the BME280 every MEASURE_PERIOD_MS
//   taskDisplay - redraws the OLED every DISPLAY_PERIOD_MS
//   taskPublish - sends the latest measurement via MQTT every MQTT_PUBLISH_PERIOD_MS
//   taskLog     - stores the latest measurement in the flash history every LOG_PERIOD_MS
// The BOOT button and the `display` console command switch the OLED on and
// off; it also switches off after a period without interaction (FR-9.x).
// A web server (web_server) shows the current values and the history on the
// local network.
// i2cMutex serializes access to the shared I2C bus, dataMutex protects the
// latest measurement shared between the tasks.
// Wi-Fi and NTP (net_time) run in the background and provide the time of day;
// telemetry holds the MQTT client. The serial console offers the `log` command
// to read the history.

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include "bme280.h"
#include "config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_console.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "history.h"
#include "net_time.h"
#include "ssd1306.h"
#include "telemetry.h"
#include "web_server.h"

static const char *TAG = "weather";

namespace {

struct SharedData {
    Bme280Data values;
    bool valid;  // false until the first successful read or after sensor loss
    TickType_t sample_tick;  // when the last successful read arrived (FR-2.6)
    time_t sample_time;      // Unix time of that read, 0 before NTP sync (FR-6.3)
};

i2c_master_bus_handle_t s_bus = nullptr;
Bme280 s_sensor;
Ssd1306 s_display;

SemaphoreHandle_t s_i2c_mutex = nullptr;   // i2cMutex
SemaphoreHandle_t s_data_mutex = nullptr;  // dataMutex
SharedData s_data = {};
TaskHandle_t s_display_task = nullptr;     // woken by taskMeasure on new data

// FR-9.1: display on/off requests from the button (ISR) and the console,
// handled by taskDisplay, which owns the display.
enum DisplayRequest : int { REQ_NONE, REQ_ON, REQ_OFF, REQ_TOGGLE };
std::atomic<int> s_display_request{REQ_NONE};
std::atomic<bool> s_display_on{true};  // current state, for `display status`

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

// FR-2.6: measurement icon, a 5x5 filled dot.
constexpr int MEASURE_ICON_W = 5;
constexpr int MEASURE_ICON_H = 5;
constexpr const char *MEASURE_ICON[MEASURE_ICON_H] = {
    ".XXX.",
    "XXXXX",
    "XXXXX",
    "XXXXX",
    ".XXX.",
};

void draw_measure_icon(int x, int y)
{
    for (int row = 0; row < MEASURE_ICON_H; row++) {
        for (int col = 0; col < MEASURE_ICON_W; col++) {
            if (MEASURE_ICON[row][col] != '.') {
                s_display.draw_pixel(x + col, y + row);
            }
        }
    }
}

void render_screen(const SharedData &data, bool measure_icon)
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
        // Measurement icon at the left edge, Wi-Fi icon at the right edge; the
        // title is centered between them. The left slot is always reserved so
        // the title does not move when the measurement icon blinks.
        if (measure_icon) {
            draw_measure_icon(0, (HEADER_H - MEASURE_ICON_H) / 2);
        }
        const int title_left = MEASURE_ICON_W + 2;
        int title_right = Ssd1306::WIDTH;
        const int wifi = net_time::wifi_level();
        if (wifi >= 0) {
            draw_wifi_icon(Ssd1306::WIDTH - WIFI_ICON_W, (HEADER_H - WIFI_ICON_H) / 2, wifi);
            title_right -= WIFI_ICON_W + 2;
        }
        // Title vertically centered in the header band.
        s_display.draw_string(title_left + (title_right - title_left - Ssd1306::text_width(title)) / 2,
                              (HEADER_H - 7) / 2, title);
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
        time_t sample_time = 0;
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
                sample_time = time(nullptr);
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
                s_data.sample_tick = xTaskGetTickCount();
                s_data.sample_time = sample_time;
            } else if (errors >= config::MAX_CONSECUTIVE_ERRORS) {
                s_data.valid = false;  // stop showing stale values
            }
            xSemaphoreGive(s_data_mutex);
        }

        // FR-2.6: redraw right away so the measurement icon appears on time.
        if (err == ESP_OK && s_display_task != nullptr) {
            xTaskNotifyGive(s_display_task);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(config::MEASURE_PERIOD_MS));
    }
}

// Asks taskDisplay to switch the display; safe from tasks only.
void request_display(DisplayRequest req)
{
    s_display_request = req;
    if (s_display_task != nullptr) {
        xTaskNotifyGive(s_display_task);
    }
}

// FR-9.1: BOOT button (active low). Presses closer than BUTTON_DEBOUNCE_MS
// to the previous one are contact bounce and ignored.
void on_button(void *)
{
    static TickType_t last_press = 0;
    const TickType_t now = xTaskGetTickCountFromISR();
    if (now - last_press < pdMS_TO_TICKS(config::BUTTON_DEBOUNCE_MS)) {
        return;
    }
    last_press = now;
    s_display_request = REQ_TOGGLE;
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_display_task, &woken);
    portYIELD_FROM_ISR(woken);
}

esp_err_t init_button()
{
    gpio_config_t io_cfg = {};
    io_cfg.pin_bit_mask = 1ULL << config::BUTTON_PIN;
    io_cfg.mode = GPIO_MODE_INPUT;
    io_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    io_cfg.intr_type = GPIO_INTR_NEGEDGE;
    esp_err_t err = gpio_config(&io_cfg);
    if (err == ESP_OK) {
        err = gpio_install_isr_service(0);
    }
    if (err == ESP_OK) {
        err = gpio_isr_handler_add(config::BUTTON_PIN, on_button, nullptr);
    }
    return err;
}

const char *request_source(int req)
{
    return req == REQ_TOGGLE ? "button" : "console";
}

void taskDisplay(void *)
{
    int errors = 0;
    const TickType_t period = pdMS_TO_TICKS(config::DISPLAY_PERIOD_MS);
    const TickType_t icon_time = pdMS_TO_TICKS(config::MEASURE_ICON_MS);
    const TickType_t auto_off = pdMS_TO_TICKS(config::DISPLAY_TIMEOUT_MS);
    TickType_t next_refresh = xTaskGetTickCount();
    TickType_t last_activity = next_refresh;  // FR-9.2: last button press or command
    bool on = true;

    while (true) {
        SharedData snapshot = {};
        if (xSemaphoreTake(s_data_mutex, mutex_timeout()) == pdTRUE) {
            snapshot = s_data;
            xSemaphoreGive(s_data_mutex);
        }

        // FR-2.6: the icon stays on for icon_time after each new measurement.
        const TickType_t now = xTaskGetTickCount();
        const TickType_t icon_age = now - snapshot.sample_tick;
        const bool measure_icon = snapshot.valid && icon_age < icon_time;

        // FR-9.1, FR-9.2: decide whether the display should be on.
        const int req = s_display_request.exchange(REQ_NONE);
        bool want_on = on;
        const char *reason = "inactivity";
        if (req != REQ_NONE) {
            last_activity = now;
            want_on = req == REQ_TOGGLE ? !on : req == REQ_ON;
            reason = request_source(req);
        } else if (on && auto_off > 0 && now - last_activity >= auto_off) {
            want_on = false;
        }

        if (!want_on) {
            if (on) {
                esp_err_t err = ESP_ERR_TIMEOUT;
                if (xSemaphoreTake(s_i2c_mutex, mutex_timeout()) == pdTRUE) {
                    err = s_display.set_power(false);  // panel sleep mode
                    xSemaphoreGive(s_i2c_mutex);
                }
                if (err == ESP_OK) {
                    on = false;
                    s_display_on = false;
                    ESP_LOGI(TAG, "Display off (%s)", reason);
                } else if (req != REQ_NONE) {
                    s_display_request = req;  // try again on the next cycle
                }
            }
            if (!on) {
                // Nothing to draw: sleep until a request or a new measurement.
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                continue;
            }
        }
        const bool turning_on = !on;

        // Rendering only touches the local frame buffer, no bus access needed.
        render_screen(snapshot, measure_icon);

        esp_err_t err = ESP_ERR_TIMEOUT;
        if (xSemaphoreTake(s_i2c_mutex, mutex_timeout()) == pdTRUE) {
            err = s_display.flush();
            // NFR-2.2: try to recover the display after repeated failures.
            if (err != ESP_OK && errors + 1 >= config::MAX_CONSECUTIVE_ERRORS) {
                ESP_LOGW(TAG, "Re-initializing SSD1306");
                if (s_display.init(s_bus, config::SSD1306_ADDR, config::I2C_FREQ_HZ) == ESP_OK) {
                    render_screen(snapshot, measure_icon);  // init() cleared the frame buffer
                    err = s_display.flush();
                }
            }
            // Switch the panel on only after the new frame is in, so the
            // stale one is never shown.
            if (err == ESP_OK && turning_on) {
                err = s_display.set_power(true);
            }
            xSemaphoreGive(s_i2c_mutex);
        } else {
            ESP_LOGW(TAG, "taskDisplay: I2C bus busy");
        }

        if (turning_on) {
            if (err == ESP_OK) {
                on = true;
                s_display_on = true;
                ESP_LOGI(TAG, "Display on (%s)", reason);
            } else if (req != REQ_NONE) {
                s_display_request = req;  // try again on the next cycle
            }
        }

        if (err == ESP_OK) {
            errors = 0;
        } else {
            errors++;
            ESP_LOGW(TAG, "Display update failed (%d in a row): %s", errors, esp_err_to_name(err));
        }

        // Sleep until the next periodic refresh (FR-2.4), until the icon has to
        // be cleared, or until taskMeasure signals new data, whichever is first.
        const TickType_t after = xTaskGetTickCount();
        if (static_cast<int32_t>(after - next_refresh) >= 0) {
            next_refresh = after + period;
        }
        TickType_t wait = next_refresh - after;
        if (measure_icon && icon_time - icon_age < wait) {
            wait = icon_time - icon_age;
        }
        ulTaskNotifyTake(pdTRUE, wait);
    }
}

// Copies the latest measurement for taskPublish and taskLog. Returns false if
// there is none or it is the one already handled (`last_tick`).
bool take_new_sample(SharedData &out, bool handled_any, TickType_t last_tick, const char *task)
{
    if (xSemaphoreTake(s_data_mutex, mutex_timeout()) != pdTRUE) {
        ESP_LOGW(TAG, "%s: data busy, skipping this cycle", task);
        return false;
    }
    out = s_data;
    xSemaphoreGive(s_data_mutex);

    if (!out.valid) {
        ESP_LOGW(TAG, "%s: no valid measurement", task);
        return false;
    }
    return !handled_any || out.sample_tick != last_tick;
}

// FR-6.2: sends the latest valid measurement to the MQTT broker. Readings that
// were already sent are not repeated, and failures only log a warning (FR-6.5).
void taskPublish(void *)
{
    TickType_t last_sent_tick = 0;
    bool sent_any = false;
    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(config::MQTT_PUBLISH_PERIOD_MS));

        SharedData snapshot;
        if (!take_new_sample(snapshot, sent_any, last_sent_tick, "taskPublish")) {
            continue;
        }

        const esp_err_t err = telemetry::publish(snapshot.values, snapshot.sample_time);
        if (err == ESP_OK) {
            last_sent_tick = snapshot.sample_tick;
            sent_any = true;
        } else {
            ESP_LOGW(TAG, "MQTT publish skipped: %s",
                     err == ESP_ERR_INVALID_STATE ? "not connected to the broker" : esp_err_to_name(err));
        }
    }
}

// FR-7.2: stores the latest valid measurement in the flash history, once.
void taskLog(void *)
{
    TickType_t last_logged_tick = 0;
    bool logged_any = false;
    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(config::LOG_PERIOD_MS));

        SharedData snapshot;
        if (!take_new_sample(snapshot, logged_any, last_logged_tick, "taskLog")) {
            continue;
        }
        const esp_err_t err = history::append(snapshot.values, snapshot.sample_time);
        if (err == ESP_OK) {
            last_logged_tick = snapshot.sample_tick;
            logged_any = true;
        } else {
            ESP_LOGW(TAG, "History write failed: %s", esp_err_to_name(err));
        }
    }
}

// FR-8.3: latest measurement for the web page.
bool current_for_web(web_server::Current &out)
{
    if (xSemaphoreTake(s_data_mutex, mutex_timeout()) != pdTRUE) {
        return false;
    }
    out.valid = s_data.valid;
    out.values = s_data.values;
    out.time = s_data.sample_time;
    out.age_ms = pdTICKS_TO_MS(xTaskGetTickCount() - s_data.sample_tick);
    xSemaphoreGive(s_data_mutex);
    return true;
}

// FR-9.1: console command `display on | off | status`.
int cmd_display(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "on") == 0) {
        request_display(REQ_ON);
    } else if (argc >= 2 && strcmp(argv[1], "off") == 0) {
        request_display(REQ_OFF);
    } else if (argc < 2 || strcmp(argv[1], "status") == 0) {
        printf("Display %s, ", s_display_on ? "on" : "off");
        if (config::DISPLAY_TIMEOUT_MS > 0) {
            printf("switches off after %lu min without activity\n",
                   static_cast<unsigned long>(config::DISPLAY_TIMEOUT_MS / 60000));
        } else {
            printf("automatic switch-off disabled\n");
        }
    } else {
        printf("Usage: display on | display off | display status\n");
        return 1;
    }
    return 0;
}

// FR-7.4: serial console with `help` and `log`, shown in `idf.py monitor`.
esp_err_t start_console(bool with_history)
{
    esp_console_repl_t *repl = nullptr;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = config::CONSOLE_PROMPT;
    repl_cfg.task_stack_size = config::CONSOLE_TASK_STACK;
    repl_cfg.task_priority = config::CONSOLE_TASK_PRIO;
    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();

    esp_err_t err = esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl);
    if (err == ESP_OK) {
        err = esp_console_register_help_command();
    }
    if (err == ESP_OK) {
        esp_console_cmd_t cmd = {};
        cmd.command = "display";
        cmd.help = "OLED display: 'on', 'off', or 'status' (state and automatic switch-off)";
        cmd.hint = "on | off | status";
        cmd.func = cmd_display;
        err = esp_console_cmd_register(&cmd);
    }
    if (err == ESP_OK && with_history) {
        err = history::register_console_command();
    }
    if (err == ESP_OK) {
        err = esp_console_start_repl(repl);
    }
    return err;
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
                    config::DISPLAY_TASK_PRIO, &s_display_task) != pdPASS) {
        enter_error_mode("Task creation failed", true);
    }

    // FR-9.1: without the button the display stays on (console still works).
    if ((err = init_button()) != ESP_OK) {
        ESP_LOGW(TAG, "BOOT button unavailable: %s", esp_err_to_name(err));
    }

    // FR-5.5: Wi-Fi/NTP problems are not critical, the station keeps running.
    err = net_time::start();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Time of day unavailable: %s", esp_err_to_name(err));
    }

    // FR-6.1, FR-6.5: MQTT needs Wi-Fi; its failures are not critical either.
    if (err == ESP_OK) {
        // FR-8.1, FR-8.6: the web page needs Wi-Fi too and is not critical.
        const esp_err_t web_err = web_server::start(current_for_web);
        if (web_err != ESP_OK) {
            ESP_LOGW(TAG, "Web page unavailable: %s", esp_err_to_name(web_err));
        }

        err = telemetry::start();
        if (err == ESP_OK) {
            if (xTaskCreate(taskPublish, "taskPublish", config::PUBLISH_TASK_STACK, nullptr,
                            config::PUBLISH_TASK_PRIO, nullptr) != pdPASS) {
                ESP_LOGW(TAG, "taskPublish creation failed, data transmission disabled");
            }
        } else if (err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Data transmission unavailable: %s", esp_err_to_name(err));
        }
    } else if (config::MQTT_BROKER_URI[0] != '\0') {
        ESP_LOGW(TAG, "MQTT broker configured but Wi-Fi is not, data transmission disabled");
    }

    // FR-7.6: the history is not critical either; without it the station runs on.
    const bool history_ok = (err = history::init()) == ESP_OK;
    if (!history_ok) {
        ESP_LOGW(TAG, "Measurement history unavailable: %s", esp_err_to_name(err));
    } else if (xTaskCreate(taskLog, "taskLog", config::LOG_TASK_STACK, nullptr,
                           config::LOG_TASK_PRIO, nullptr) != pdPASS) {
        ESP_LOGW(TAG, "taskLog creation failed, measurement history disabled");
    }

    if ((err = start_console(history_ok)) != ESP_OK) {
        ESP_LOGW(TAG, "Serial console unavailable: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Running: measure every %lu ms, display every %lu ms",
             static_cast<unsigned long>(config::MEASURE_PERIOD_MS),
             static_cast<unsigned long>(config::DISPLAY_PERIOD_MS));
}
