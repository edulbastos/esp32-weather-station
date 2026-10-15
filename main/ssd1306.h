// SSD1306 OLED driver (I2C) with a local frame buffer.
// Drawing functions only touch RAM; flush() sends the buffer to the panel,
// so the I2C bus is only needed during init() and flush().
#pragma once

#include <cstdint>
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "config.h"

class Ssd1306 {
public:
    static constexpr int WIDTH  = config::DISPLAY_WIDTH;
    static constexpr int HEIGHT = config::DISPLAY_HEIGHT;
    static constexpr int PAGES  = HEIGHT / 8;

    esp_err_t init(i2c_master_bus_handle_t bus, uint8_t address, uint32_t scl_speed_hz);
    void deinit();

    // Sends the frame buffer to the display.
    esp_err_t flush();
    esp_err_t set_contrast(uint8_t contrast);
    esp_err_t set_power(bool on);

    void clear();
    void draw_pixel(int x, int y, bool on = true);
    void draw_hline(int x, int y, int w, bool on = true);
    // Draws a character / string with the 5x7 font, magnified by `scale`.
    // Returns the x position after the last drawn character.
    int draw_char(int x, int y, char c, int scale = 1);
    int draw_string(int x, int y, const char *s, int scale = 1);
    // Width in pixels of a string drawn with draw_string().
    static int text_width(const char *s, int scale = 1);

private:
    esp_err_t send_commands(const uint8_t *cmds, size_t len);

    i2c_master_dev_handle_t dev_ = nullptr;
    // tx_[0] is the I2C control byte (0x40 = data), followed by the frame buffer.
    uint8_t tx_[1 + WIDTH * PAGES] = {};
    uint8_t *const fb_ = &tx_[1];
};
