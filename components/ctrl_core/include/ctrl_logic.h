#pragma once

#include "ctrl_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fill params with project defaults: 35/37/40 degC and documented timings. */
void ctrl_set_defaults(ctrl_params_t *p);

/* Validate a threshold pair against params. start and stop must be finite,
 * inside [min_c, max_c], satisfy start < stop and stop <= overtemp_c. */
bool ctrl_validate_thresholds(const ctrl_params_t *p, float start_c, float stop_c);

void ctrl_init(ctrl_state_t *s, const ctrl_params_t *p);

/* Validate then atomically apply a new threshold pair. Returns false and
 * leaves the old thresholds untouched when validation fails. */
bool ctrl_apply_thresholds(ctrl_state_t *s, float start_c, float stop_c, uint32_t now_ms);

/* Feed one sensor sample. valid=false (or an out-of-range / non-finite
 * value) marks the sensor as failed immediately. */
void ctrl_on_sample(ctrl_state_t *s, bool valid, float celsius, uint32_t now_ms);

/* Periodic evaluation: staleness, protections, heating decision. Must be
 * called with a monotonic millisecond time base. */
void ctrl_tick(ctrl_state_t *s, uint32_t now_ms);

/* Manual commands. Return false when the request is refused (protection
 * active / no valid sensor data). ctrl_manual_off is always accepted and
 * latches the output off until the mode changes. */
bool ctrl_manual_on(ctrl_state_t *s, uint32_t now_ms);
bool ctrl_manual_off(ctrl_state_t *s, uint32_t now_ms);
bool ctrl_restore_auto(ctrl_state_t *s, uint32_t now_ms);

/* Enter the maintenance safe-lock (no heating at all). */
void ctrl_enter_safe_locked(ctrl_state_t *s, uint32_t now_ms);

/* Forced re-evaluation helper used by the fault manager. */
void ctrl_force_prot(ctrl_state_t *s, ctrl_prot_t prot, uint32_t now_ms);

bool ctrl_output(const ctrl_state_t *s);
ctrl_mode_t ctrl_mode(const ctrl_state_t *s);
ctrl_prot_t ctrl_prot(const ctrl_state_t *s);

/* Protocol enum mappings. */
const char *ctrl_mode_str(ctrl_mode_t m);        /* "auto" | "manual" */
const char *ctrl_relay_status_str(bool heating); /* "heating" | "off" */
const char *ctrl_prot_str(ctrl_prot_t p);

/* ---- 4线 PWM 风扇状态(纯逻辑,host 可测) ---- */
typedef enum {
    FAN_STATUS_OFF = 0,      /* 占空比 0 且已停转(或惯性余转尚在宽限期内) */
    FAN_STATUS_OK = 1,       /* 正常:有转速,或占空比>0 尚在启动宽限期内 */
    FAN_STATUS_STALL = 2,    /* 堵转:占空比>0 且持续无转速脉冲 */
    FAN_STATUS_OVERRUN = 3,  /* 失控空转:占空比 0 但持续旋转超宽限期(PWM 未生效) */
} fan_status_t;

/* 风扇状态判定:
 * - duty==0:off_spin_ms(占空比 0 且持续有转速的时长)未达 spindown_ms
 *   视为停机惯性余转(off),达到则判定失控空转(overrun,PWM 未生效);
 * - duty>0 且 rpm>0:ok;
 * - duty>0 且 rpm==0:连续零转速时长 zero_ms 未达 stall_ms 视为启动
 *   宽限期(ok),达到则判定堵转(stall)。
 * stall_ms / spindown_ms 由调用方注入,便于单测。 */
fan_status_t ctrl_fan_status_eval(uint8_t duty_pct, uint32_t rpm, uint32_t zero_ms,
                                  uint32_t off_spin_ms, uint32_t stall_ms, uint32_t spindown_ms);

const char *ctrl_fan_status_str(fan_status_t st); /* "off" | "ok" | "stall" | "overrun" */

#ifdef __cplusplus
}
#endif
