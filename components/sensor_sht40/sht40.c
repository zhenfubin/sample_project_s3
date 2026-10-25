#include "sht40.h"

#include <inttypes.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sht40";

/* SHT40 器件参数:I2C 地址 / 单次测量命令(高精度)/ 测量等待时间 / 量程 */
#define SHT40_ADDR          0x44
#define SHT40_CMD_MEASURE_H 0xFD /* 单次测量,高重复精度模式 */
#define SHT40_MEASURE_WAIT_MS 10 /* 发命令后等待转换完成 */
#define SHT40_TEMP_MIN      (-40.0f) /* 数据手册温度下限 */
#define SHT40_TEMP_MAX      (125.0f) /* 数据手册温度上限 */

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static int64_t s_last_ok_us; /* 最近一次有效采样的时间戳(微秒) */
static uint32_t s_failures;  /* 连续失败计数(故障管理用) */

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
    ESP_LOGI(TAG, "SHT40 已初始化: SDA=%d SCL=%d addr=0x%02X", cfg->sda_gpio, cfg->scl_gpio,
             SHT40_ADDR);
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
        ESP_LOGW(TAG, "测量命令发送失败(连续失败 %" PRIu32 " 次): %s", s_failures,
                 esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(SHT40_MEASURE_WAIT_MS));

    uint8_t raw[6];
    memset(raw, 0, sizeof(raw));
    err = i2c_master_receive(s_dev, raw, sizeof(raw), 100);
    if (err != ESP_OK) {
        s_failures++;
        ESP_LOGW(TAG, "测量数据读取失败(连续失败 %" PRIu32 " 次): %s", s_failures,
                 esp_err_to_name(err));
        return err;
    }

    if (crc8(&raw[0], 2) != raw[2] || crc8(&raw[3], 2) != raw[5]) {
        s_failures++;
        ESP_LOGW(TAG, "CRC 校验失败(连续失败 %" PRIu32 " 次),本次采样丢弃", s_failures);
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
        ESP_LOGW(TAG, "温度超量程(连续失败 %" PRIu32 " 次): T=%.2fC", s_failures, (double)t);
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
