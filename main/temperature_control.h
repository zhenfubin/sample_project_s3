#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sensor + control + telemetry task cluster, event-driven (spec §4).
 * Commands arrive as APP_EVENT_MQTT_MESSAGE; all publishes go through the
 * mqtt_manager pump. The relay GPIO is only written through this module. */
typedef struct {
    const char *device_id;       /* 设备 ID,如 s3-thermostat-xxxxxx */
    int sensor_index;        /* 主题传感器编号,0 = SHT40 */
    bool start_safe_locked;  /* 启动安全锁未解 → 强制维护模式 */
    float start_c;           /* 初始加热启动阈值(℃) */
    float stop_c;            /* 初始加热停止阈值(℃) */
    int fan_pwm_gpio;        /* 4线风扇 PWM 调速引脚,-1=禁用调速 */
    int fan_tach_gpio;       /* 4线风扇测速引脚,-1=禁用测速 */
    uint8_t fan_default_duty; /* 风扇上电初始占空比(%),不持久化 */
    int fan_index;           /* 主题风扇编号,0 = fans/0 */
} temperature_control_cfg_t;

esp_err_t temperature_control_start(const temperature_control_cfg_t *cfg);

/* Safety override used by the health monitor: forces the relay off and
 * latches SAFE_LOCKED in the control state machine. */
void temperature_control_emergency_off(void);

#ifdef __cplusplus
}
#endif
