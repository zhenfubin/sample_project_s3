#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SHT40 digital humidity/temperature sensor driver (I2C master).
 *  - every bus transaction is bounded by a timeout
 *  - every read is CRC-8 checked; a bad CRC counts as a failed read
 *  - tracks the last valid sample timestamp and consecutive failures so the
 *    fault manager can latch a sensor fault
 */
typedef struct {
    uint8_t sda_gpio;         /* I2C SDA 引脚 */
    uint8_t scl_gpio;         /* I2C SCL 引脚 */
    uint32_t i2c_timeout_ms;  /* 单次总线事务超时(毫秒) */
} sht40_config_t;

esp_err_t sht40_init(const sht40_config_t *cfg);
void sht40_deinit(void);

/* Trigger a measurement and read it out. Returns ESP_OK only when the
 * sample is complete, CRC-valid and in plausible range. */
esp_err_t sht40_read(float *temperature_c, float *humidity_rh);

int64_t sht40_last_ok_us(void);
uint32_t sht40_consecutive_failures(void);

#ifdef __cplusplus
}
#endif
