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

#ifdef __cplusplus
}
#endif
