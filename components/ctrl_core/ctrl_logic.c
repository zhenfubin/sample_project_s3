#include "ctrl_logic.h"

#include <math.h>
#include <string.h>

#include "ctrl_protocol.h" /* CTRL_LIMIT_MIN_C / CTRL_LIMIT_MAX_C */

#define COOLDOWN_AFTER_FAULT(p) ((p)->cooldown_ms)

static void set_prot(ctrl_state_t *s, ctrl_prot_t prot, uint32_t now_ms)
{
    if (s->prot != prot) {
        s->prot = prot;
        s->prot_since_ms = now_ms;
    }
}

void ctrl_set_defaults(ctrl_params_t *p)
{
    p->start_c = 35.0f;
    p->stop_c = 37.0f;
    p->overtemp_c = 40.0f;
    p->min_c = CTRL_LIMIT_MIN_C;
    p->max_c = CTRL_LIMIT_MAX_C;
    p->sample_period_ms = 2000;
    p->sensor_stale_ms = 8000;
    p->manual_max_ms = 2 * 60 * 60 * 1000;
    p->heat_max_ms = 2 * 60 * 60 * 1000;
    p->cooldown_ms = 30 * 1000;
    p->timeout_retry_ms = 10 * 60 * 1000;
    p->safe_lock_ms = 30 * 60 * 1000;
    p->sensor_recover_samples = 3;
}

bool ctrl_validate_thresholds(const ctrl_params_t *p, float start_c, float stop_c)
{
    if (!isfinite(start_c) || !isfinite(stop_c)) {
        return false;
    }
    if (start_c < p->min_c || start_c > p->max_c) {
        return false;
    }
    if (stop_c < p->min_c || stop_c > p->max_c) {
        return false;
    }
    if (!(start_c < stop_c)) {
        return false;
    }
    if (stop_c > p->overtemp_c) {
        return false;
    }
    return true;
}

void ctrl_init(ctrl_state_t *s, const ctrl_params_t *p)
{
    memset(s, 0, sizeof(*s));
    s->params = *p;
    s->mode = CTRL_MODE_AUTO;
    s->prot = CTRL_PROT_NONE;
}

bool ctrl_apply_thresholds(ctrl_state_t *s, float start_c, float stop_c, uint32_t now_ms)
{
    (void)now_ms;
    if (!ctrl_validate_thresholds(&s->params, start_c, stop_c)) {
        return false;
    }
    /* Atomic apply: both values become effective at once. */
    s->params.start_c = start_c;
    s->params.stop_c = stop_c;
    return true;
}

void ctrl_on_sample(ctrl_state_t *s, bool valid, float celsius, uint32_t now_ms)
{
    s->have_sample = true;
    s->last_sample_ms = now_ms;
    if (valid && isfinite(celsius) && celsius >= s->params.min_c && celsius <= s->params.max_c) {
        s->sensor_valid = true;
        s->celsius = celsius;
        s->last_valid_ms = now_ms;
        if (s->valid_streak < UINT32_MAX) {
            s->valid_streak++;
        }
        s->invalid_streak = 0;
    } else {
        s->sensor_valid = false;
        s->invalid_streak++;
        s->valid_streak = 0;
    }
}

static void update_protections(ctrl_state_t *s, uint32_t now_ms)
{
    const ctrl_params_t *p = &s->params;

    if (!s->sensor_valid) {
        /* SAFE_LOCKED is never downgraded by a sensor fault. */
        if (s->prot != CTRL_PROT_SAFE_LOCKED) {
            set_prot(s, CTRL_PROT_FAULT_SENSOR, now_ms);
        }
        return;
    }

    switch (s->prot) {
    case CTRL_PROT_SAFE_LOCKED:
        if ((now_ms - s->prot_since_ms) >= p->safe_lock_ms &&
            s->valid_streak >= p->sensor_recover_samples && s->celsius <= p->stop_c) {
            set_prot(s, CTRL_PROT_COOLDOWN, now_ms);
        }
        break;
    case CTRL_PROT_FAULT_SENSOR:
        if (s->valid_streak >= p->sensor_recover_samples && s->celsius < p->overtemp_c) {
            set_prot(s, CTRL_PROT_COOLDOWN, now_ms);
        }
        break;
    case CTRL_PROT_FAULT_OVERTEMP:
        if (s->celsius <= p->stop_c) {
            set_prot(s, CTRL_PROT_COOLDOWN, now_ms);
        }
        break;
    case CTRL_PROT_FAULT_TIMEOUT:
        if (s->celsius >= p->stop_c) {
            set_prot(s, CTRL_PROT_COOLDOWN, now_ms);
        } else if ((now_ms - s->prot_since_ms) >= p->timeout_retry_ms &&
                   s->celsius < p->overtemp_c) {
            set_prot(s, CTRL_PROT_COOLDOWN, now_ms);
        }
        break;
    case CTRL_PROT_COOLDOWN:
        if ((now_ms - s->prot_since_ms) >= p->cooldown_ms) {
            set_prot(s, CTRL_PROT_NONE, now_ms);
        }
        break;
    default:
        break;
    }

    /* Over-temperature has priority over everything except SAFE_LOCKED. */
    if (s->prot != CTRL_PROT_SAFE_LOCKED && s->celsius >= p->overtemp_c) {
        set_prot(s, CTRL_PROT_FAULT_OVERTEMP, now_ms);
    }
}

void ctrl_tick(ctrl_state_t *s, uint32_t now_ms)
{
    const ctrl_params_t *p = &s->params;
    bool want = s->heating;

    if (s->sensor_valid && (now_ms - s->last_sample_ms) > p->sensor_stale_ms) {
        s->sensor_valid = false;
        s->invalid_streak++;
        s->valid_streak = 0;
    }

    update_protections(s, now_ms);

    if (s->prot != CTRL_PROT_NONE) {
        want = false;
    } else {
        switch (s->mode) {
        case CTRL_MODE_AUTO:
            if (!s->sensor_valid) {
                want = false;
            } else if (s->celsius < p->start_c) {
                want = true;
            } else if (s->celsius >= p->stop_c) {
                want = false;
            }
            /* Inside [start, stop) the previous state is kept (hysteresis). */
            break;
        case CTRL_MODE_MANUAL_ON:
            if (!s->sensor_valid) {
                want = false;
            } else if (now_ms >= s->manual_deadline_ms) {
                /* Manual heating ran out: fall back to AUTO behind a cooldown
                 * so the output cannot restart within the same cycle. */
                s->mode = CTRL_MODE_AUTO;
                set_prot(s, CTRL_PROT_COOLDOWN, now_ms);
                want = false;
            } else {
                want = true;
            }
            break;
        case CTRL_MODE_MANUAL_OFF:
        default:
            want = false;
            break;
        }
    }

    if (want && !s->heating) {
        s->heat_start_ms = now_ms;
    }
    if (want && (now_ms - s->heat_start_ms) >= p->heat_max_ms) {
        set_prot(s, CTRL_PROT_FAULT_TIMEOUT, now_ms);
        want = false;
    }
    s->heating = want;
}

bool ctrl_manual_on(ctrl_state_t *s, uint32_t now_ms)
{
    if (s->prot != CTRL_PROT_NONE || !s->sensor_valid) {
        return false;
    }
    s->mode = CTRL_MODE_MANUAL_ON;
    s->manual_deadline_ms = now_ms + s->params.manual_max_ms;
    return true;
}

bool ctrl_manual_off(ctrl_state_t *s, uint32_t now_ms)
{
    (void)now_ms;
    s->mode = CTRL_MODE_MANUAL_OFF;
    s->heating = false;
    return true;
}

bool ctrl_restore_auto(ctrl_state_t *s, uint32_t now_ms)
{
    s->mode = CTRL_MODE_AUTO;
    /* Recovery path: clear retryable protections when the situation allows. */
    if (s->sensor_valid && s->celsius < s->params.overtemp_c) {
        if (s->prot == CTRL_PROT_FAULT_TIMEOUT) {
            set_prot(s, CTRL_PROT_COOLDOWN, now_ms);
        } else if (s->prot == CTRL_PROT_SAFE_LOCKED &&
                   (now_ms - s->prot_since_ms) >= s->params.safe_lock_ms &&
                   s->celsius <= s->params.stop_c) {
            set_prot(s, CTRL_PROT_COOLDOWN, now_ms);
        }
    }
    return true;
}

void ctrl_enter_safe_locked(ctrl_state_t *s, uint32_t now_ms)
{
    set_prot(s, CTRL_PROT_SAFE_LOCKED, now_ms);
    s->heating = false;
}

void ctrl_force_prot(ctrl_state_t *s, ctrl_prot_t prot, uint32_t now_ms)
{
    set_prot(s, prot, now_ms);
    if (prot != CTRL_PROT_NONE) {
        s->heating = false;
    }
}

bool ctrl_output(const ctrl_state_t *s)
{
    return s->heating;
}

ctrl_mode_t ctrl_mode(const ctrl_state_t *s)
{
    return s->mode;
}

ctrl_prot_t ctrl_prot(const ctrl_state_t *s)
{
    return s->prot;
}

const char *ctrl_mode_str(ctrl_mode_t m)
{
    switch (m) {
    case CTRL_MODE_AUTO:
        return "auto";
    case CTRL_MODE_MANUAL_ON:
    case CTRL_MODE_MANUAL_OFF:
    default:
        return "manual";
    }
}

const char *ctrl_relay_status_str(bool heating)
{
    return heating ? "heating" : "off";
}

const char *ctrl_prot_str(ctrl_prot_t p)
{
    switch (p) {
    case CTRL_PROT_NONE:
        return "none";
    case CTRL_PROT_COOLDOWN:
        return "cooldown";
    case CTRL_PROT_FAULT_SENSOR:
        return "fault_sensor";
    case CTRL_PROT_FAULT_OVERTEMP:
        return "fault_overtemp";
    case CTRL_PROT_FAULT_TIMEOUT:
        return "fault_timeout";
    case CTRL_PROT_SAFE_LOCKED:
    default:
        return "safe_locked";
    }
}

fan_status_t ctrl_fan_status_eval(uint8_t duty_pct, uint32_t rpm, uint32_t zero_ms,
                                  uint32_t off_spin_ms, uint32_t stall_ms, uint32_t spindown_ms)
{
    if (duty_pct == 0) {
        /* 指令停转:惯性余转仍算 off;持续旋转超宽限期说明 PWM 未生效,
         * 实际转速与指令矛盾,判定失控空转(不得继续谎报 off) */
        return (rpm > 0 && off_spin_ms >= spindown_ms) ? FAN_STATUS_OVERRUN : FAN_STATUS_OFF;
    }
    if (rpm > 0) {
        return FAN_STATUS_OK;
    }
    /* 占空比>0 但无脉冲:未到判定时长视为启动宽限期,超时判定堵转 */
    return (zero_ms >= stall_ms) ? FAN_STATUS_STALL : FAN_STATUS_OK;
}

const char *ctrl_fan_status_str(fan_status_t st)
{
    switch (st) {
    case FAN_STATUS_OFF:
        return "off";
    case FAN_STATUS_STALL:
        return "stall";
    case FAN_STATUS_OVERRUN:
        return "overrun";
    case FAN_STATUS_OK:
    default:
        return "ok";
    }
}
