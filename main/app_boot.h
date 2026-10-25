#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Boot / recovery state machine states (task book section 3). */
typedef enum {
    BOOT_STATE_SAFE = 0,
    BOOT_STATE_INIT_NVS,
    BOOT_STATE_LOAD_CONFIG,
    BOOT_STATE_INIT_DRIVERS,
    BOOT_STATE_INIT_CONTROL,
    BOOT_STATE_INIT_LED,
    BOOT_STATE_INIT_WIFI,
    BOOT_STATE_DISCOVERY,
    BOOT_STATE_INIT_MQTT,
    BOOT_STATE_SYNC_STATE,
    BOOT_STATE_RUNNING,
    BOOT_STATE_DEGRADED,
    BOOT_STATE_RECOVERY,
    BOOT_STATE_SAFE_LOCKED,
} boot_state_t;

typedef enum {
    GUARD_NORMAL = 0,    /* start normally */
    GUARD_SAFE_LOCKED,   /* crash loop detected: heating disabled */
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
