#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Operating mode. MANUAL_ON / MANUAL_OFF are kept distinct internally and
 * both map to "manual" in the MQTT payload. */
typedef enum {
    CTRL_MODE_AUTO = 0,
    CTRL_MODE_MANUAL_ON,
    CTRL_MODE_MANUAL_OFF,
} ctrl_mode_t;

/* Protection states. Any protection other than NONE forces heating off and
 * has priority over every manual / auto heating request. */
typedef enum {
    CTRL_PROT_NONE = 0,
    CTRL_PROT_COOLDOWN,
    CTRL_PROT_FAULT_SENSOR,
    CTRL_PROT_FAULT_OVERTEMP,
    CTRL_PROT_FAULT_TIMEOUT,
    CTRL_PROT_SAFE_LOCKED,
} ctrl_prot_t;

typedef struct {
    float start_c;                    /* 加热启动阈值(℃),如 35 */
    float stop_c;                     /* 加热停止阈值(℃),如 37 */
    float overtemp_c;                 /* 软件超温保护阈值(℃),如 40 */
    float min_c;                      /* 采样/阈值允许的最低值 */
    float max_c;                      /* 采样/阈值允许的最高值 */
    uint32_t sample_period_ms;        /* 采样周期(毫秒),默认 2000 */
    uint32_t sensor_stale_ms;         /* 采样超过该时长视为过期 */
    uint32_t manual_max_ms;           /* 手动加热最长持续时间 */
    uint32_t heat_max_ms;             /* 任意模式下连续加热最长时间 */
    uint32_t cooldown_ms;             /* 故障解除后的冷却等待时间 */
    uint32_t timeout_retry_ms;        /* FAULT_TIMEOUT 后重试等待时间 */
    uint32_t safe_lock_ms;            /* SAFE_LOCKED 最短保持时间 */
    uint32_t sensor_recover_samples;  /* 清除 FAULT_SENSOR 所需连续有效采样数 */
} ctrl_params_t;

typedef struct {
    ctrl_params_t params;       /* 当前生效的控制参数 */
    ctrl_mode_t mode;           /* 运行模式:自动/手动开/手动关 */
    ctrl_prot_t prot;           /* 当前保护状态(NONE = 正常) */
    uint32_t prot_since_ms;     /* 进入当前保护状态的时刻 */
    bool heating;               /* 控制器输出的继电器指令状态 */
    bool have_sample;           /* 是否已收到过采样 */
    bool sensor_valid;          /* 最近一次采样是否有效 */
    float celsius;              /* 最近温度值(℃) */
    uint32_t last_sample_ms;    /* 最近一次采样时刻(含无效采样) */
    uint32_t last_valid_ms;     /* 最近一次有效采样时刻 */
    uint32_t valid_streak;      /* 连续有效采样计数 */
    uint32_t invalid_streak;    /* 连续无效采样计数 */
    uint32_t heat_start_ms;     /* 本轮加热开始时刻 */
    uint32_t manual_deadline_ms; /* 手动加热截止时刻(超时保护) */
} ctrl_state_t;

#ifdef __cplusplus
}
#endif
