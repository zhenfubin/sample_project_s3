/**
 * @file fan_ctrl.c
 * @brief 4线 PWM 风扇驱动实现:LEDC 25kHz PWM 调速 + PCNT 转速测量。
 *
 * 转速窗口固定 1s,每窗口清零计数;rpm = 脉冲数 × 60 / (每转脉冲数 × 窗口秒)。
 * 转速>0 时连续零转速计时清零;占空比变化时也清零(重启启动宽限期)。
 * 另统计 duty==0 却持续有转速的时长,供失控空转(overrun)判定。
 */
#include "fan_ctrl.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "fan_ctrl";

#define FAN_PWM_FREQ_HZ_DEFAULT 25000u /* 4线风扇规范 PWM 频率 */
#define FAN_TACH_PPR_DEFAULT 2u        /* 标准 4线风扇每转 2 个脉冲 */
#define FAN_TACH_WINDOW_MS 1000u       /* 测速窗口:1s */
#define FAN_TACH_GLITCH_NS 1000        /* 输入毛刺滤波:1us */
#define FAN_PWM_DUTY_MAX 1023u         /* LEDC 10-bit 分辨率满占空比 */

static bool s_inited;
static bool s_pwm_en; /* PWM 是否启用 */
static volatile uint8_t s_duty_pct;
static volatile uint32_t s_rpm;
static volatile uint32_t s_zero_ms;     /* 连续零转速时长(ms) */
static volatile uint32_t s_off_spin_ms; /* duty==0 但持续有转速的时长(ms) */
static uint8_t s_ppr = FAN_TACH_PPR_DEFAULT;
static pcnt_unit_handle_t s_pcnt;
static esp_timer_handle_t s_tach_timer;

/* 1s 周期测速回调:读取并清零脉冲计数,折算 rpm,维护零转速计时 */
static void tach_timer_cb(void *arg)
{
    (void)arg;
    int count = 0;
    if (s_pcnt != NULL && pcnt_unit_get_count(s_pcnt, &count) == ESP_OK) {
        pcnt_unit_clear_count(s_pcnt);
    }
    if (count < 0) {
        count = 0;
    }
    s_rpm = (uint32_t)count * 60u / ((uint32_t)s_ppr * (FAN_TACH_WINDOW_MS / 1000u));
    if (count == 0) {
        s_zero_ms += FAN_TACH_WINDOW_MS;
    } else {
        s_zero_ms = 0;
    }
    /* duty==0 却持续有转速:累计失控空转时长(供 overrun 判定),其余情况清零 */
    if (count > 0 && s_duty_pct == 0) {
        s_off_spin_ms += FAN_TACH_WINDOW_MS;
    } else {
        s_off_spin_ms = 0;
    }
}

esp_err_t fan_ctrl_init(const fan_ctrl_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_inited) {
        return ESP_OK;
    }
    if (cfg->pwm_gpio < 0 && cfg->tach_gpio < 0) {
        ESP_LOGW(TAG, "风扇 PWM 与测速引脚均未配置,风扇功能禁用");
        return ESP_ERR_INVALID_STATE;
    }

    /* ---- PWM 输出(LEDC,10-bit,默认 25kHz) ---- */
    if (cfg->pwm_gpio >= 0) {
        uint32_t freq = cfg->pwm_freq_hz ? cfg->pwm_freq_hz : FAN_PWM_FREQ_HZ_DEFAULT;
        ledc_timer_config_t tcfg = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_10_BIT,
            .timer_num = LEDC_TIMER_0,
            .freq_hz = freq,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        esp_err_t err = ledc_timer_config(&tcfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "LEDC 定时器配置失败: %s", esp_err_to_name(err));
            return err;
        }
        ledc_channel_config_t ccfg = {
            .gpio_num = cfg->pwm_gpio,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_0,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0, /* 安全默认:上电即停转 */
            .hpoint = 0,
        };
        err = ledc_channel_config(&ccfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "LEDC 通道配置失败: %s", esp_err_to_name(err));
            return err;
        }
        s_pwm_en = true;
        ESP_LOGI(TAG, "风扇PWM已初始化: GPIO%d, %luHz, 10-bit", cfg->pwm_gpio,
                 (unsigned long)freq);
    }

    /* ---- 转速测量(PCNT,1s 窗口) ---- */
    if (cfg->tach_gpio >= 0) {
        s_ppr = cfg->tach_pulses_per_rev ? cfg->tach_pulses_per_rev : FAN_TACH_PPR_DEFAULT;
        pcnt_unit_config_t ucfg = {
            .high_limit = 10000,
            .low_limit = -10000,
        };
        esp_err_t err = pcnt_new_unit(&ucfg, &s_pcnt);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "脉冲计数单元创建失败: %s", esp_err_to_name(err));
            return err;
        }
        /* 毛刺滤波:窄于 1us 的脉冲忽略(须在 enable 之前配置) */
        pcnt_glitch_filter_config_t gcfg = {
            .max_glitch_ns = FAN_TACH_GLITCH_NS,
        };
        err = pcnt_unit_set_glitch_filter(s_pcnt, &gcfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "脉冲计数滤波配置失败: %s", esp_err_to_name(err));
            return err;
        }
        pcnt_chan_config_t chcfg = {
            .edge_gpio_num = cfg->tach_gpio,
            .level_gpio_num = -1,
        };
        pcnt_channel_handle_t chan = NULL;
        err = pcnt_new_channel(s_pcnt, &chcfg, &chan);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "脉冲计数通道创建失败: %s", esp_err_to_name(err));
            return err;
        }
        /* TACH 开漏输出:内部弱上拉兜底(外部仍需 10K 上拉至 3.3V);
         * 上升沿计数,每脉冲一个上升沿 */
        gpio_set_pull_mode((gpio_num_t)cfg->tach_gpio, GPIO_PULLUP_ONLY);
        err = pcnt_channel_set_edge_action(chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                           PCNT_CHANNEL_EDGE_ACTION_HOLD);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "脉冲计数边沿配置失败: %s", esp_err_to_name(err));
            return err;
        }
        pcnt_unit_enable(s_pcnt);
        pcnt_unit_clear_count(s_pcnt);
        pcnt_unit_start(s_pcnt);

        esp_timer_create_args_t targs = {
            .callback = tach_timer_cb,
            .name = "fan_tach",
        };
        err = esp_timer_create(&targs, &s_tach_timer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "测速定时器创建失败: %s", esp_err_to_name(err));
            return err;
        }
        err = esp_timer_start_periodic(s_tach_timer, FAN_TACH_WINDOW_MS * 1000);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "测速定时器启动失败: %s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "风扇测速已初始化: GPIO%d, %u脉冲/转, 1s窗口", cfg->tach_gpio,
                 (unsigned)s_ppr);
    }

    s_inited = true;
    if (cfg->initial_duty_pct > 0) {
        fan_ctrl_set_duty(cfg->initial_duty_pct);
    }
    return ESP_OK;
}

esp_err_t fan_ctrl_set_duty(uint8_t duty_pct)
{
    if (!s_inited || !s_pwm_en) {
        return ESP_ERR_INVALID_STATE;
    }
    if (duty_pct > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t duty_val = (uint32_t)duty_pct * FAN_PWM_DUTY_MAX / 100u;
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty_val);
    if (err != ESP_OK) {
        return err;
    }
    err = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    if (err != ESP_OK) {
        return err;
    }
    s_duty_pct = duty_pct;
    s_zero_ms = 0;     /* 占空比变化:重启启动宽限期/堵转计时 */
    s_off_spin_ms = 0; /* 占空比变化:重启停机惯性宽限期 */
    return ESP_OK;
}

uint8_t fan_ctrl_get_duty(void)
{
    return s_duty_pct;
}

uint32_t fan_ctrl_get_rpm(void)
{
    return s_rpm;
}

uint32_t fan_ctrl_rpm_zero_ms(void)
{
    return s_zero_ms;
}

uint32_t fan_ctrl_off_spin_ms(void)
{
    return s_off_spin_ms;
}
