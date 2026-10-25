#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Tasks whose progress is monitored by the health/watchdog supervisor. */
typedef enum {
    APP_HEALTH_SENSOR = 0,
    APP_HEALTH_CONTROL,
    APP_HEALTH_NET,
    APP_HEALTH_COUNT,
} app_health_task_t;

/*
 * Watchdog & health monitor:
 *  - each managed task bumps a progress counter when it completes real work
 *  - the monitor feeds the task watchdog only after verifying progress
 *  - escalating response to a stalled task: force relay off -> SAFE_LOCKED
 *    -> controlled restart (only after persistent unresponsiveness)
 */
void app_health_start(bool safe_locked);
void app_health_note(app_health_task_t who);

#ifdef __cplusplus
}
#endif
