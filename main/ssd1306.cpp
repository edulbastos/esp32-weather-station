#include "ssd1306.h"

#include <cstring>
#include "esp_log.h"
#include "font5x7.h"

static const char *TAG = "ssd1306";

static_assert(Ssd1306::HEIGHT == 64 || Ssd1306::HEIGHT == 32, "DISPLAY_HEIGHT must be 32 or 64");

namespace {
constexpr uint8_t CONTROL_CMD  = 0x00;
constexpr uint8_t CONTROL_DATA = 0x40;
constexpr int     MAX_CMD_LEN  = 32;
}  // namespace

esp_err_t Ssd1306::send_commands(const uint8_t *cmds, size_t len)
{
    if (len > MAX_CMD_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t buf[1 + MAX_CMD_LEN];
    buf[0] = CONTROL_CMD;
    std::memcpy(&buf[1], cmds, len);
    return i2c_master_transmit(dev_, buf, len + 1, config::I2C_TIMEOUT_MS);
}

esp_err_t Ssd1306::init(i2c_master_bus_handle_t bus, uint8_t address, uint32_t scl_speed_hz)
{
    deinit();

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = address;
    dev_cfg.scl_speed_hz    = scl_speed_hz;

    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &dev_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add device 0x%02X: %s", address, esp_err_to_name(err));
        return err;
    }

    const uint8_t init_cmds[] = {
        0xAE,                                   // display off
        0xD5, 0x80,                             // clock divide ratio / oscillator
        0xA8, static_cast<uint8_t>(HEIGHT - 1), // multiplex ratio
        0xD3, 0x00,                             // display offset
        0x40,                                   // start line 0
        0x8D, 0x14,                             // charge pump on
        0x20, 0x00,                             // horizontal addressing mode
        0xA1,                                   // segment remap (column 127 -> SEG0)
        0xC8,                                   // COM scan direction remapped
        0xDA, static_cast<uint8_t>(HEIGHT == 64 ? 0x12 : 0x02),  // COM pins
        0x81, config::DISPLAY_CONTRAST,         // contrast
        0xD9, 0xF1,                             // pre-charge period
        0xDB, 0x40,                             // VCOMH deselect level
        0xA4,                                   // display RAM content
        0xA6,                                   // normal (not inverted)
        0x2E,                                   // deactivate scroll
    };
    err = send_commands(init_cmds, sizeof(init_cmds));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No response at address 0x%02X: %s", address, esp_err_to_name(err));
        deinit();
        return err;
    }

    clear();
    err = flush();
    if (err == ESP_OK) {
        err = set_power(true);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Initialization failed: %s", esp_err_to_name(err));
        deinit();
        return err;
    }

    ESP_LOGI(TAG, "SSD1306 %dx%d initialized at 0x%02X", WIDTH, HEIGHT, address);
    return ESP_OK;
}

void Ssd1306::deinit()
{
    if (dev_ != nullptr) {
        i2c_master_bus_rm_device(dev_);
        dev_ = nullptr;
    }
}

esp_err_t Ssd1306::flush()
{
    if (dev_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t window[] = {
        0x21, 0x00, static_cast<uint8_t>(WIDTH - 1),  // column range
        0x22, 0x00, static_cast<uint8_t>(PAGES - 1),  // page range
    };
    esp_err_t err = send_commands(window, sizeof(window));
    if (err != ESP_OK) {
        return err;
    }
    tx_[0] = CONTROL_DATA;
    return i2c_master_transmit(dev_, tx_, sizeof(tx_), config::I2C_TIMEOUT_MS);
}

esp_err_t Ssd1306::set_contrast(uint8_t contrast)
{
    const uint8_t cmds[] = {0x81, contrast};
    return send_commands(cmds, sizeof(cmds));
}

esp_err_t Ssd1306::set_power(bool on)
{
    const uint8_t cmd = on ? 0xAF : 0xAE;
    return send_commands(&cmd, 1);
}

void Ssd1306::clear()
{
    std::memset(fb_, 0, WIDTH * PAGES);
}

void Ssd1306::draw_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) {
        return;
    }
    uint8_t &byte = fb_[(y / 8) * WIDTH + x];
    const uint8_t bit = static_cast<uint8_t>(1u << (y % 8));
    byte = on ? (byte | bit) : (byte & ~bit);
}

void Ssd1306::draw_hline(int x, int y, int w, bool on)
{
    for (int i = 0; i < w; i++) {
        draw_pixel(x + i, y, on);
    }
}

int Ssd1306::draw_char(int x, int y, char c, int scale)
{
    if (c < FONT5X7_FIRST_CHAR || c > FONT5X7_LAST_CHAR) {
        c = '?';
    }
    const uint8_t *glyph = FONT5X7[c - FONT5X7_FIRST_CHAR];
    for (int col = 0; col < FONT5X7_WIDTH; col++) {
        for (int row = 0; row < FONT5X7_HEIGHT; row++) {
            if (!(glyph[col] & (1u << row))) {
                continue;
            }
            for (int dx = 0; dx < scale; dx++) {
                for (int dy = 0; dy < scale; dy++) {
                    draw_pixel(x + col * scale + dx, y + row * scale + dy);
                }
            }
        }
    }
    return x + (FONT5X7_WIDTH + 1) * scale;  // one column spacing
}

int Ssd1306::draw_string(int x, int y, const char *s, int scale)
{
    while (*s != '\0') {
        x = draw_char(x, y, *s++, scale);
    }
    return x;
}

int Ssd1306::text_width(const char *s, int scale)
{
    const int n = static_cast<int>(std::strlen(s));
    // Last character has no trailing spacing column.
    return n == 0 ? 0 : n * (FONT5X7_WIDTH + 1) * scale - scale;
}
