/**
 * @file fan_ctrl.h
 * @brief 4线 PWM 风扇驱动:PWM 调速(LEDC)+ 转速测量(PULSE_CNT)。
 *
 * 4线风扇引脚定义(标准 PC 风扇):
 *   1=GND(黑) 2=+12V(黄) 3=TACH 转速(绿,开漏) 4=PWM 调速(蓝)
 *
 * 安全约定:
 *   - 风扇电源来自外部 12V,与开发板共地;禁止用开发板供电。
 *   - TACH 上拉到 3.3V(禁止 5V 上拉,ESP32-S3 GPIO 非 5V 耐受)。
 *   - PWM 25kHz,详见 docs/specs/ 风扇接入规范。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 4线 PWM 风扇驱动配置 */
typedef struct {
    int pwm_gpio;                /* PWM 输出引脚(风扇第4脚),-1=禁用调速 */
    int tach_gpio;               /* 测速输入引脚(风扇第3脚),-1=禁用测速 */
    uint32_t pwm_freq_hz;        /* PWM 频率,规范 25kHz;0=取默认值 */
    uint8_t tach_pulses_per_rev; /* 每转脉冲数,标准=2;0=取默认值 */
    uint8_t initial_duty_pct;    /* 上电初始占空比 0-100,推荐 0 */
} fan_ctrl_config_t;

/* 初始化风扇驱动(幂等)。pwm_gpio 与 tach_gpio 均 <0 时返回 ESP_ERR_INVALID_STATE */
esp_err_t fan_ctrl_init(const fan_ctrl_config_t *cfg);

/* 设置 PWM 占空比 0-100(%),0=停转。每次占空比变化会重启堵转计时 */
esp_err_t fan_ctrl_set_duty(uint8_t duty_pct);

/* 当前占空比(%) */
uint8_t fan_ctrl_get_duty(void);

/* 最近测速窗口(1s)折算的转速(rpm);未启用测速时恒为 0 */
uint32_t fan_ctrl_get_rpm(void);

/* 连续零转速时长(ms);转速>0 或占空比变化时清零。未启用测速时恒为 0 */
uint32_t fan_ctrl_rpm_zero_ms(void);

/* 占空比 0 但持续有转速的时长(ms),用于失控空转(overrun)判定;
 * 占空比>0、转速归零或占空比变化时清零。未启用测速时恒为 0 */
uint32_t fan_ctrl_off_spin_ms(void);

#ifdef __cplusplus
}
#endif
