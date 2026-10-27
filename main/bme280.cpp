#include "bme280.h"

#include <cmath>
#include <cstring>
#include "config.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bme280";

namespace {
constexpr uint8_t REG_CALIB_00  = 0x88;  // 0x88..0xA1
constexpr uint8_t REG_CHIP_ID   = 0xD0;
constexpr uint8_t REG_RESET     = 0xE0;
constexpr uint8_t REG_CALIB_26  = 0xE1;  // 0xE1..0xE7
constexpr uint8_t REG_CTRL_HUM  = 0xF2;
constexpr uint8_t REG_STATUS    = 0xF3;
constexpr uint8_t REG_CTRL_MEAS = 0xF4;
constexpr uint8_t REG_CONFIG    = 0xF5;
constexpr uint8_t REG_DATA      = 0xF7;  // 0xF7..0xFE

constexpr uint8_t CHIP_ID_BME280 = 0x60;
constexpr uint8_t RESET_CMD      = 0xB6;
constexpr uint8_t STATUS_IM_UPDATE = 0x01;

// Oversampling x1 for humidity, temperature and pressure; normal mode.
constexpr uint8_t CTRL_HUM_VALUE  = 0x01;                              // osrs_h = x1
constexpr uint8_t CTRL_MEAS_VALUE = (0x01 << 5) | (0x01 << 2) | 0x03;  // osrs_t, osrs_p = x1, normal
// Standby 250 ms, IIR filter coefficient 4.
constexpr uint8_t CONFIG_VALUE    = (0x03 << 5) | (0x02 << 2);

// Raw values reported when a measurement was skipped
constexpr int32_t ADC_SKIPPED_TP = 0x80000;
constexpr int32_t ADC_SKIPPED_H  = 0x8000;

inline uint16_t le_u16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline int16_t  le_s16(const uint8_t *p) { return static_cast<int16_t>(le_u16(p)); }
}  // namespace

esp_err_t Bme280::write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(dev_, buf, sizeof(buf), config::I2C_TIMEOUT_MS);
}

esp_err_t Bme280::read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(dev_, &reg, 1, data, len, config::I2C_TIMEOUT_MS);
}

esp_err_t Bme280::init(i2c_master_bus_handle_t bus, uint8_t address, uint32_t scl_speed_hz)
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

    uint8_t chip_id = 0;
    err = read_regs(REG_CHIP_ID, &chip_id, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No response at address 0x%02X: %s", address, esp_err_to_name(err));
        deinit();
        return err;
    }
    if (chip_id != CHIP_ID_BME280) {
        ESP_LOGE(TAG, "Unexpected chip ID 0x%02X (expected 0x%02X%s)", chip_id, CHIP_ID_BME280,
                 chip_id == 0x58 ? ", this is a BMP280 without humidity" : "");
        deinit();
        return ESP_ERR_NOT_FOUND;
    }

    // Soft reset, then wait until the NVM calibration data has been copied.
    err = write_reg(REG_RESET, RESET_CMD);
    if (err != ESP_OK) {
        deinit();
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
    for (int i = 0; i < 10; i++) {
        uint8_t status = 0;
        err = read_regs(REG_STATUS, &status, 1);
        if (err == ESP_OK && !(status & STATUS_IM_UPDATE)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    err = read_calibration();
    if (err == ESP_OK) {
        // ctrl_hum only becomes effective after a write to ctrl_meas.
        err = write_reg(REG_CTRL_HUM, CTRL_HUM_VALUE);
    }
    if (err == ESP_OK) {
        err = write_reg(REG_CONFIG, CONFIG_VALUE);
    }
    if (err == ESP_OK) {
        err = write_reg(REG_CTRL_MEAS, CTRL_MEAS_VALUE);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Configuration failed: %s", esp_err_to_name(err));
        deinit();
        return err;
    }

    // Allow the first conversion to complete.
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI(TAG, "BME280 initialized at 0x%02X", address);
    return ESP_OK;
}

void Bme280::deinit()
{
    if (dev_ != nullptr) {
        i2c_master_bus_rm_device(dev_);
        dev_ = nullptr;
    }
}

esp_err_t Bme280::read_calibration()
{
    uint8_t c[26];
    esp_err_t err = read_regs(REG_CALIB_00, c, sizeof(c));
    if (err != ESP_OK) {
        return err;
    }
    cal_.t1 = le_u16(&c[0]);
    cal_.t2 = le_s16(&c[2]);
    cal_.t3 = le_s16(&c[4]);
    cal_.p1 = le_u16(&c[6]);
    cal_.p2 = le_s16(&c[8]);
    cal_.p3 = le_s16(&c[10]);
    cal_.p4 = le_s16(&c[12]);
    cal_.p5 = le_s16(&c[14]);
    cal_.p6 = le_s16(&c[16]);
    cal_.p7 = le_s16(&c[18]);
    cal_.p8 = le_s16(&c[20]);
    cal_.p9 = le_s16(&c[22]);
    cal_.h1 = c[25];  // 0xA1

    uint8_t h[7];
    err = read_regs(REG_CALIB_26, h, sizeof(h));
    if (err != ESP_OK) {
        return err;
    }
    cal_.h2 = le_s16(&h[0]);
    cal_.h3 = h[2];
    cal_.h4 = static_cast<int16_t>((static_cast<int8_t>(h[3]) * 16) | (h[4] & 0x0F));
    cal_.h5 = static_cast<int16_t>((static_cast<int8_t>(h[5]) * 16) | (h[4] >> 4));
    cal_.h6 = static_cast<int8_t>(h[6]);

    // FR-10.3: 0x88..0x9F, 0xA1 and 0xE1..0xE7 (0xA0 is not documented).
    uint8_t trim[32];
    memcpy(trim, c, 24);
    trim[24] = c[25];
    memcpy(trim + 25, h, sizeof(h));
    fingerprint_ = esp_rom_crc32_le(0, trim, sizeof(trim));
    return ESP_OK;
}

esp_err_t Bme280::read(Bme280Data &out)
{
    if (dev_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    // Burst read of all data registers guarantees a consistent sample.
    uint8_t d[8];
    esp_err_t err = read_regs(REG_DATA, d, sizeof(d));
    if (err != ESP_OK) {
        return err;
    }

    const int32_t adc_p = (static_cast<int32_t>(d[0]) << 12) | (d[1] << 4) | (d[2] >> 4);
    const int32_t adc_t = (static_cast<int32_t>(d[3]) << 12) | (d[4] << 4) | (d[5] >> 4);
    const int32_t adc_h = (static_cast<int32_t>(d[6]) << 8) | d[7];

    if (adc_t == ADC_SKIPPED_TP || adc_p == ADC_SKIPPED_TP || adc_h == ADC_SKIPPED_H) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Temperature must be compensated first: it sets t_fine_.
    out.temperature_c = compensate_temperature(adc_t) / 100.0f;
    out.pressure_hpa  = compensate_pressure(adc_p) / 256.0f / 100.0f;
    out.humidity_rh   = compensate_humidity(adc_h) / 1024.0f;
    return ESP_OK;
}

float Bme280::altitude_m(float pressure_hpa, float sea_level_hpa)
{
    return 44330.0f * (1.0f - std::pow(pressure_hpa / sea_level_hpa, 0.1903f));
}

// --- Bosch BME280 datasheet compensation formulas (section 4.2.3) ----------

int32_t Bme280::compensate_temperature(int32_t adc_t)
{
    int32_t var1 = ((((adc_t >> 3) - (static_cast<int32_t>(cal_.t1) << 1))) *
                    static_cast<int32_t>(cal_.t2)) >> 11;
    int32_t var2 = (((((adc_t >> 4) - static_cast<int32_t>(cal_.t1)) *
                      ((adc_t >> 4) - static_cast<int32_t>(cal_.t1))) >> 12) *
                    static_cast<int32_t>(cal_.t3)) >> 14;
    t_fine_ = var1 + var2;
    return (t_fine_ * 5 + 128) >> 8;
}

uint32_t Bme280::compensate_pressure(int32_t adc_p)
{
    int64_t var1 = static_cast<int64_t>(t_fine_) - 128000;
    int64_t var2 = var1 * var1 * static_cast<int64_t>(cal_.p6);
    var2 = var2 + ((var1 * static_cast<int64_t>(cal_.p5)) << 17);
    var2 = var2 + (static_cast<int64_t>(cal_.p4) << 35);
    var1 = ((var1 * var1 * static_cast<int64_t>(cal_.p3)) >> 8) +
           ((var1 * static_cast<int64_t>(cal_.p2)) << 12);
    var1 = (((static_cast<int64_t>(1) << 47) + var1) * static_cast<int64_t>(cal_.p1)) >> 33;
    if (var1 == 0) {
        return 0;  // avoid division by zero
    }
    int64_t p = 1048576 - adc_p;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (static_cast<int64_t>(cal_.p9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (static_cast<int64_t>(cal_.p8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (static_cast<int64_t>(cal_.p7) << 4);
    return static_cast<uint32_t>(p);
}

uint32_t Bme280::compensate_humidity(int32_t adc_h)
{
    int32_t v = t_fine_ - 76800;
    v = (((((adc_h << 14) - (static_cast<int32_t>(cal_.h4) << 20) -
            (static_cast<int32_t>(cal_.h5) * v)) + 16384) >> 15) *
         (((((((v * static_cast<int32_t>(cal_.h6)) >> 10) *
              (((v * static_cast<int32_t>(cal_.h3)) >> 11) + 32768)) >> 10) + 2097152) *
               static_cast<int32_t>(cal_.h2) + 8192) >> 14));
    v = v - (((((v >> 15) * (v >> 15)) >> 7) * static_cast<int32_t>(cal_.h1)) >> 4);
    v = v < 0 ? 0 : v;
    v = v > 419430400 ? 419430400 : v;
    return static_cast<uint32_t>(v >> 12);
}
