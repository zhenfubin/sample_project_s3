/* Host unit tests for ctrl_core control logic (pure C, no ESP-IDF). */

#include "ctrl_logic.h"
#include "test_util.h"

static void init(ctrl_state_t *s, ctrl_params_t *p)
{
    ctrl_set_defaults(p);
    ctrl_init(s, p);
}

static void test_hysteresis(void)
{
    ctrl_state_t s;
    ctrl_params_t p;
    init(&s, &p);

    /* 36 C is inside [35,37): must stay off. */
    ctrl_on_sample(&s, true, 36.0f, 0);
    ctrl_tick(&s, 0);
    CHECK(!ctrl_output(&s));

    /* Below start -> heating on. */
    ctrl_on_sample(&s, true, 34.9f, 1000);
    ctrl_tick(&s, 1000);
    CHECK(ctrl_output(&s));

    /* Back inside [35,37): stays on (hysteresis). */
    ctrl_on_sample(&s, true, 36.0f, 2000);
    ctrl_tick(&s, 2000);
    CHECK(ctrl_output(&s));

    /* At stop -> off. */
    ctrl_on_sample(&s, true, 37.0f, 3000);
    ctrl_tick(&s, 3000);
    CHECK(!ctrl_output(&s));

    /* Inside [35,37) again: stays off. */
    ctrl_on_sample(&s, true, 36.0f, 4000);
    ctrl_tick(&s, 4000);
    CHECK(!ctrl_output(&s));
}

static void test_threshold_validation(void)
{
    ctrl_state_t s;
    ctrl_params_t p;
    init(&s, &p);

    CHECK(ctrl_validate_thresholds(&p, 35.0f, 37.0f));
    CHECK(!ctrl_validate_thresholds(&p, 37.0f, 35.0f));  /* start >= stop */
    CHECK(!ctrl_validate_thresholds(&p, 35.0f, 35.0f));  /* equal */
    CHECK(!ctrl_validate_thresholds(&p, 35.0f, 41.0f));  /* stop > overtemp */
    CHECK(!ctrl_validate_thresholds(&p, NAN, 37.0f));
    CHECK(!ctrl_validate_thresholds(&p, 35.0f, INFINITY));
    CHECK(!ctrl_validate_thresholds(&p, -500.0f, 37.0f));

    /* Failed apply leaves old thresholds untouched. */
    CHECK(ctrl_apply_thresholds(&s, 30.0f, 32.0f, 0));
    CHECK(!ctrl_apply_thresholds(&s, 32.0f, 30.0f, 0));
    CHECK(s.params.start_c == 30.0f && s.params.stop_c == 32.0f);
}

static void test_sensor_fault_and_recovery(void)
{
    ctrl_state_t s;
    ctrl_params_t p;
    init(&s, &p);

    ctrl_on_sample(&s, true, 34.0f, 0);
    ctrl_tick(&s, 0);
    CHECK(ctrl_output(&s));

    /* A single invalid sample latches FAULT_SENSOR and cuts heating. */
    ctrl_on_sample(&s, false, 0, 1000);
    ctrl_tick(&s, 1000);
    CHECK(!ctrl_output(&s));
    CHECK(ctrl_prot(&s) == CTRL_PROT_FAULT_SENSOR);

    /* Manual heating must be refused while the fault is active. */
    CHECK(!ctrl_manual_on(&s, 1000));

    /* One good sample is not enough to recover. */
    ctrl_on_sample(&s, true, 34.0f, 2000);
    ctrl_tick(&s, 2000);
    CHECK(ctrl_prot(&s) == CTRL_PROT_FAULT_SENSOR);

    ctrl_on_sample(&s, true, 34.0f, 4000);
    ctrl_tick(&s, 4000);
    ctrl_on_sample(&s, true, 34.0f, 6000);
    ctrl_tick(&s, 6000);
    CHECK(ctrl_prot(&s) == CTRL_PROT_COOLDOWN);

    /* Cooldown keeps heating off until it expires. */
    ctrl_tick(&s, 7000);
    CHECK(!ctrl_output(&s));
    ctrl_on_sample(&s, true, 34.0f, 37000);
    ctrl_tick(&s, 37000); /* 6000 + cooldown + 1000 */
    CHECK(ctrl_prot(&s) == CTRL_PROT_NONE);
    CHECK(ctrl_output(&s)); /* 34 C < start */
}

static void test_stale_sample(void)
{
    ctrl_state_t s;
    ctrl_params_t p;
    init(&s, &p);

    ctrl_on_sample(&s, true, 34.0f, 0);
    ctrl_tick(&s, 0);
    CHECK(ctrl_output(&s));

    /* No new sample for a long time -> stale -> fault -> heating off. */
    ctrl_tick(&s, p.sensor_stale_ms + 5000);
    CHECK(!ctrl_output(&s));
    CHECK(ctrl_prot(&s) == CTRL_PROT_FAULT_SENSOR);
}

static void test_overtemp(void)
{
    ctrl_state_t s;
    ctrl_params_t p;
    init(&s, &p);

    ctrl_on_sample(&s, true, 34.0f, 0);
    ctrl_tick(&s, 0);
    CHECK(ctrl_output(&s));

    ctrl_on_sample(&s, true, 40.5f, 1000);
    ctrl_tick(&s, 1000);
    CHECK(!ctrl_output(&s));
    CHECK(ctrl_prot(&s) == CTRL_PROT_FAULT_OVERTEMP);

    /* Manual ON must be refused while over-temperature. */
    CHECK(!ctrl_manual_on(&s, 1000));

    /* Not cleared until temperature is back at or below stop. */
    ctrl_on_sample(&s, true, 38.0f, 2000);
    ctrl_tick(&s, 2000);
    CHECK(ctrl_prot(&s) == CTRL_PROT_FAULT_OVERTEMP);

    ctrl_on_sample(&s, true, 36.5f, 3000);
    ctrl_tick(&s, 3000);
    CHECK(ctrl_prot(&s) == CTRL_PROT_COOLDOWN);
}

static void test_manual_mode(void)
{
    ctrl_state_t s;
    ctrl_params_t p;
    init(&s, &p);

    ctrl_on_sample(&s, true, 36.5f, 0); /* inside band, auto would be off */
    ctrl_tick(&s, 0);
    CHECK(!ctrl_output(&s));

    /* Manual ON wins over the auto hysteresis. */
    CHECK(ctrl_manual_on(&s, 1000));
    ctrl_tick(&s, 1000);
    CHECK(ctrl_output(&s));
    CHECK(ctrl_mode(&s) == CTRL_MODE_MANUAL_ON);

    /* Manual OFF always wins. */
    CHECK(ctrl_manual_off(&s, 2000));
    ctrl_tick(&s, 2000);
    CHECK(!ctrl_output(&s));

    /* Manual ON again; over-temperature must override it. */
    CHECK(ctrl_manual_on(&s, 3000));
    ctrl_on_sample(&s, true, 41.0f, 3000);
    ctrl_tick(&s, 3000);
    CHECK(!ctrl_output(&s));
    CHECK(ctrl_prot(&s) == CTRL_PROT_FAULT_OVERTEMP);
}

static void test_manual_expiry(void)
{
    ctrl_state_t s;
    ctrl_params_t p;
    init(&s, &p);

    ctrl_on_sample(&s, true, 36.5f, 0);
    CHECK(ctrl_manual_on(&s, 1000));
    ctrl_tick(&s, 2000);
    CHECK(ctrl_output(&s));

    /* After manual_max_ms the device falls back to AUTO behind a cooldown. */
    uint32_t t = 1000 + p.manual_max_ms;
    ctrl_on_sample(&s, true, 36.5f, t);
    ctrl_tick(&s, t + 1);
    CHECK(!ctrl_output(&s));
    CHECK(ctrl_mode(&s) == CTRL_MODE_AUTO);
    CHECK(ctrl_prot(&s) == CTRL_PROT_COOLDOWN);
}

static void test_continuous_heat_timeout(void)
{
    ctrl_state_t s;
    ctrl_params_t p;
    init(&s, &p);

    ctrl_on_sample(&s, true, 30.0f, 0); /* well below start: heating on */
    ctrl_tick(&s, 0);
    CHECK(ctrl_output(&s));

    /* Keep feeding valid samples so only the heating timer can trip. */
    for (uint32_t t = 2000; t + 2000 <= p.heat_max_ms; t += 2000) {
        ctrl_on_sample(&s, true, 30.0f, t);
        ctrl_tick(&s, t);
        CHECK(ctrl_output(&s));
    }

    /* Continuous heating beyond heat_max_ms -> FAULT_TIMEOUT. */
    ctrl_on_sample(&s, true, 30.0f, p.heat_max_ms);
    ctrl_tick(&s, p.heat_max_ms + 1000);
    CHECK(!ctrl_output(&s));
    CHECK(ctrl_prot(&s) == CTRL_PROT_FAULT_TIMEOUT);

    /* Temperature recovered: fault clears into cooldown. */
    ctrl_on_sample(&s, true, 37.5f, p.heat_max_ms + 2000);
    ctrl_tick(&s, p.heat_max_ms + 2000);
    CHECK(ctrl_prot(&s) == CTRL_PROT_COOLDOWN);
}

static void test_safe_locked(void)
{
    ctrl_state_t s;
    ctrl_params_t p;
    init(&s, &p);

    ctrl_on_sample(&s, true, 30.0f, 0);
    ctrl_enter_safe_locked(&s, 1000);
    ctrl_tick(&s, 1000);
    CHECK(!ctrl_output(&s));
    CHECK(ctrl_prot(&s) == CTRL_PROT_SAFE_LOCKED);
    CHECK(!ctrl_manual_on(&s, 1000));

    /* Even below start with valid samples, SAFE_LOCKED persists until its
     * minimum time has elapsed and the temperature is <= stop. */
    ctrl_tick(&s, 1000 + p.safe_lock_ms - 1);
    CHECK(ctrl_prot(&s) == CTRL_PROT_SAFE_LOCKED);

    for (uint32_t t = 1000 + p.safe_lock_ms; t < 1000 + p.safe_lock_ms + 10000; t += 2000) {
        ctrl_on_sample(&s, true, 36.5f, t);
        ctrl_tick(&s, t);
    }
    CHECK(ctrl_prot(&s) != CTRL_PROT_SAFE_LOCKED);
    CHECK(!ctrl_output(&s)); /* still cooling down first */
}

static void test_no_sample_no_heating(void)
{
    ctrl_state_t s;
    ctrl_params_t p;
    init(&s, &p);

    /* Without any valid sample the relay must never heat. */
    ctrl_tick(&s, 0);
    ctrl_tick(&s, 5000);
    CHECK(!ctrl_output(&s));
    CHECK(!ctrl_manual_on(&s, 5000));
}

int main(void)
{
    test_hysteresis();
    test_threshold_validation();
    test_sensor_fault_and_recovery();
    test_stale_sample();
    test_overtemp();
    test_manual_mode();
    test_manual_expiry();
    test_continuous_heat_timeout();
    test_safe_locked();
    test_no_sample_no_heating();
    TEST_REPORT("ctrl_logic");
}
