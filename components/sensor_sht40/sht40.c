#include "sht40.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SHT40_ADDR          0x44
#define SHT40_CMD_MEASURE_H 0xFD /* single shot, high repeatability */
#define SHT40_MEASURE_WAIT_MS 10
#define SHT40_TEMP_MIN      (-40.0f)
#define SHT40_TEMP_MAX      (125.0f)

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static int64_t s_last_ok_us;
static uint32_t s_failures;

static uint8_t crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

esp_err_t sht40_init(const sht40_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = cfg->sda_gpio,
        .scl_io_num = cfg->scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHT40_ADDR,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
        return err;
    }
    s_failures = 0;
    return ESP_OK;
}

void sht40_deinit(void)
{
    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    if (s_bus) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
}

esp_err_t sht40_read(float *temperature_c, float *humidity_rh)
{
    if (s_dev == NULL || temperature_c == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t cmd = SHT40_CMD_MEASURE_H;
    esp_err_t err = i2c_master_transmit(s_dev, &cmd, 1, 100);
    if (err != ESP_OK) {
        s_failures++;
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(SHT40_MEASURE_WAIT_MS));

    uint8_t raw[6];
    memset(raw, 0, sizeof(raw));
    err = i2c_master_receive(s_dev, raw, sizeof(raw), 100);
    if (err != ESP_OK) {
        s_failures++;
        return err;
    }

    if (crc8(&raw[0], 2) != raw[2] || crc8(&raw[3], 2) != raw[5]) {
        s_failures++;
        return ESP_ERR_INVALID_CRC;
    }

    uint16_t raw_t = (uint16_t)((raw[0] << 8) | raw[1]);
    uint16_t raw_h = (uint16_t)((raw[3] << 8) | raw[4]);

    float t = -45.0f + 175.0f * ((float)raw_t / 65535.0f);
    float rh = -6.0f + 125.0f * ((float)raw_h / 65535.0f);
    if (rh < 0.0f) {
        rh = 0.0f;
    } else if (rh > 100.0f) {
        rh = 100.0f;
    }

    if (!(t >= SHT40_TEMP_MIN && t <= SHT40_TEMP_MAX)) {
        s_failures++;
        return ESP_ERR_INVALID_RESPONSE;
    }

    *temperature_c = t;
    if (humidity_rh) {
        *humidity_rh = rh;
    }
    s_last_ok_us = esp_timer_get_time();
    s_failures = 0;
    return ESP_OK;
}

int64_t sht40_last_ok_us(void)
{
    return s_last_ok_us;
}

uint32_t sht40_consecutive_failures(void)
{
    return s_failures;
}
