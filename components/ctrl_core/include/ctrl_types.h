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
    float start_c;                    /* heating start threshold, e.g. 35 */
    float stop_c;                     /* heating stop threshold, e.g. 37 */
    float overtemp_c;                 /* software over-temperature limit, e.g. 40 */
    float min_c;                      /* lowest plausible sample / threshold */
    float max_c;                      /* highest plausible sample / threshold */
    uint32_t sample_period_ms;        /* sensor sampling period, 2000 */
    uint32_t sensor_stale_ms;         /* sample older than this is expired */
    uint32_t manual_max_ms;           /* max continuous manual heating */
    uint32_t heat_max_ms;             /* max continuous heating (any mode) */
    uint32_t cooldown_ms;             /* pause after a fault clears */
    uint32_t timeout_retry_ms;        /* pause after FAULT_TIMEOUT before retry */
    uint32_t safe_lock_ms;            /* minimum time spent in SAFE_LOCKED */
    uint32_t sensor_recover_samples;  /* consecutive valid samples to clear FAULT_SENSOR */
} ctrl_params_t;

typedef struct {
    ctrl_params_t params;
    ctrl_mode_t mode;
    ctrl_prot_t prot;
    uint32_t prot_since_ms;
    bool heating;                     /* commanded relay output state */
    bool have_sample;
    bool sensor_valid;
    float celsius;
    uint32_t last_sample_ms;          /* last sample, valid or not */
    uint32_t last_valid_ms;
    uint32_t valid_streak;
    uint32_t invalid_streak;
    uint32_t heat_start_ms;
    uint32_t manual_deadline_ms;
} ctrl_state_t;

#ifdef __cplusplus
}
#endif
