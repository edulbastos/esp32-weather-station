// BME280 driver (temperature, humidity, pressure) on top of the
// ESP-IDF I2C master driver. Uses the Bosch datasheet integer
// compensation formulas.
#pragma once

#include <cstdint>
#include "driver/i2c_master.h"
#include "esp_err.h"

struct Bme280Data {
    float temperature_c;  // degrees Celsius
    float humidity_rh;    // percent relative humidity
    float pressure_hpa;   // hectopascal
};

class Bme280 {
public:
    // Adds the device to the bus, checks the chip ID, reads the calibration
    // data and starts continuous (normal mode) measurements.
    esp_err_t init(i2c_master_bus_handle_t bus, uint8_t address, uint32_t scl_speed_hz);

    // Reads and compensates the latest measurement.
    esp_err_t read(Bme280Data &out);

    // Releases the I2C device handle.
    void deinit();

    // Altitude estimate (m) from pressure and reference sea-level pressure.
    static float altitude_m(float pressure_hpa, float sea_level_hpa);

private:
    struct Calibration {
        uint16_t t1; int16_t t2, t3;
        uint16_t p1; int16_t p2, p3, p4, p5, p6, p7, p8, p9;
        uint8_t  h1; int16_t h2; uint8_t h3; int16_t h4, h5; int8_t h6;
    };

    esp_err_t write_reg(uint8_t reg, uint8_t value);
    esp_err_t read_regs(uint8_t reg, uint8_t *data, size_t len);
    esp_err_t read_calibration();

    int32_t  compensate_temperature(int32_t adc_t);  // 0.01 degC
    uint32_t compensate_pressure(int32_t adc_p);     // Pa in Q24.8
    uint32_t compensate_humidity(int32_t adc_h);     // %RH in Q22.10

    i2c_master_dev_handle_t dev_ = nullptr;
    Calibration cal_ = {};
    int32_t t_fine_ = 0;
};
