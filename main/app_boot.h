#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Boot / recovery state machine states (task book section 3). */
typedef enum {
    BOOT_STATE_SAFE = 0,       /* 初始安全态 */
    BOOT_STATE_INIT_NVS,       /* 初始化 NVS */
    BOOT_STATE_LOAD_CONFIG,    /* 加载配置 */
    BOOT_STATE_INIT_DRIVERS,   /* 初始化驱动(继电器/I2C) */
    BOOT_STATE_INIT_CONTROL,   /* 启动控制任务簇 */
    BOOT_STATE_INIT_LED,       /* 启动状态灯 */
    BOOT_STATE_INIT_WIFI,      /* 启动 Wi-Fi */
    BOOT_STATE_DISCOVERY,      /* 启动发现 */
    BOOT_STATE_INIT_MQTT,      /* 启动 MQTT */
    BOOT_STATE_SYNC_STATE,     /* 等待时间同步/上报状态 */
    BOOT_STATE_RUNNING,        /* 正常运行 */
    BOOT_STATE_DEGRADED,       /* 降级运行 */
    BOOT_STATE_RECOVERY,       /* 恢复模式 */
    BOOT_STATE_SAFE_LOCKED,    /* 安全锁定(禁止加热) */
} boot_state_t;

typedef enum {
    GUARD_NORMAL = 0,    /* 正常启动 */
    GUARD_SAFE_LOCKED,   /* 检测到崩溃循环:禁止加热 */
} guard_decision_t;

/* Log the hardware reset reason in human-readable form. */
void boot_log_reset_reason(void);

/* Restart-loop guard: counts resets in RTC memory. A session that stayed
 * healthy for 5 minutes resets the counter; 5 fast resets force
 * GUARD_SAFE_LOCKED (maintenance mode, no heating). */
guard_decision_t boot_guard_check(void);

/* Called once after 5 minutes of stable operation. */
void boot_guard_mark_healthy(void);

/* Clear the guard state (explicit unlock, e.g. after maintenance). */
void boot_guard_reset(void);

bool boot_guard_lockout_active(void);

void boot_set_state(boot_state_t st);
boot_state_t boot_get_state(void);
const char *boot_state_str(boot_state_t st);

#ifdef __cplusplus
}
#endif
