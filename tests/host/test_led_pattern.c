/* Host tests for the pure-logic LED pattern engine (RGB LED status spec V1.0).
 *
 * Covers spec §7 acceptance tests 1-9 at the logic level and test 10's
 * purity/no-side-effect property (the engine never touches hardware).
 */
#include "test_util.h"

#include "led_pattern.h"

static led_state_t state(led_wifi_state_t w, led_mqtt_state_t m, led_sensor_state_t s,
                         bool control, bool fault, bool ready)
{
    led_state_t st = {
        .wifi = w,
        .mqtt = m,
        .sensor = s,
        .control_active = control,
        .fault = fault,
        .ready = ready,
    };
    return st;
}

static void check_pattern(led_display_t d, const led_seg_t *segs, unsigned n)
{
    const led_pattern_t *p = led_pattern_get(d);
    CHECK(p->n_segs == n);
    for (unsigned i = 0; i < n && i < p->n_segs; i++) {
        CHECK(p->segs[i].color == segs[i].color);
        CHECK(p->segs[i].dur_ms == segs[i].dur_ms);
    }
}

static void test_pattern_timings(void)
{
    /* Spec §2 table: every blink rhythm is an exact acceptance requirement. */
    const led_seg_t l0[] = { { LED_COLOR_BLUE, 200 }, { LED_COLOR_OFF, 800 } };
    const led_seg_t l1[] = { { LED_COLOR_BLUE, 200 }, { LED_COLOR_OFF, 200 } };
    const led_seg_t l2[] = { { LED_COLOR_PURPLE, 600 }, { LED_COLOR_OFF, 600 } };
    const led_seg_t l3[] = {
        { LED_COLOR_YELLOW, 150 }, { LED_COLOR_OFF, 150 },
        { LED_COLOR_YELLOW, 150 }, { LED_COLOR_OFF, 1200 },
    };
    const led_seg_t l4[] = { { LED_COLOR_GREEN, 0 } };
    const led_seg_t l5[] = { { LED_COLOR_GREEN, 150 }, { LED_COLOR_OFF, 1850 } };
    const led_seg_t l6[] = {
        { LED_COLOR_RED, 150 }, { LED_COLOR_OFF, 150 }, { LED_COLOR_RED, 150 },
        { LED_COLOR_OFF, 150 }, { LED_COLOR_RED, 150 }, { LED_COLOR_OFF, 900 },
    };
    check_pattern(LED_DISPLAY_L0_INIT, l0, 2);
    check_pattern(LED_DISPLAY_L1_WIFI, l1, 2);
    check_pattern(LED_DISPLAY_L2_MQTT, l2, 2);
    check_pattern(LED_DISPLAY_L3_NET_DOWN, l3, 4);
    check_pattern(LED_DISPLAY_L4_IDLE, l4, 1);
    check_pattern(LED_DISPLAY_L5_ACTIVE, l5, 2);
    check_pattern(LED_DISPLAY_L6_FAULT, l6, 6);
}

static void test_acceptance_1_startup(void)
{
    /* 1. Power-on / init: blue slow blink. Sensor UNKNOWN is not a fault. */
    led_state_t s = state(LED_WIFI_INIT, LED_MQTT_INIT, LED_SENSOR_UNKNOWN, false, false, false);
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L0_INIT);
    s.ready = true;
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L0_INIT); /* still L0: no IP yet */
}

static void test_acceptance_2_wifi_connecting(void)
{
    /* 2. Wi-Fi connecting: blue fast blink. */
    led_state_t s = state(LED_WIFI_CONNECTING, LED_MQTT_INIT, LED_SENSOR_UNKNOWN, false, false, false);
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L1_WIFI);
    s.mqtt = LED_MQTT_CONNECTING;
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L1_WIFI); /* L1 beats L2 */
}

static void test_acceptance_3_mqtt_connecting(void)
{
    /* 3. Wi-Fi up, MQTT connecting: purple slow blink. */
    led_state_t s = state(LED_WIFI_GOT_IP, LED_MQTT_CONNECTING, LED_SENSOR_OK, false, false, false);
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L2_MQTT);
    s.mqtt = LED_MQTT_INIT; /* waiting for network, not connected yet */
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L2_MQTT);
}

static void test_acceptance_4_net_down(void)
{
    /* 4. Network down (link lost before reconnect starts): yellow double blink. */
    led_state_t s = state(LED_WIFI_DOWN, LED_MQTT_INIT, LED_SENSOR_OK, false, false, true);
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L3_NET_DOWN);
    s = state(LED_WIFI_GOT_IP, LED_MQTT_DOWN, LED_SENSOR_OK, false, false, true);
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L3_NET_DOWN);
    /* L3 outranks control action display. */
    s = state(LED_WIFI_DOWN, LED_MQTT_CONNECTED, LED_SENSOR_OK, true, false, true);
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L3_NET_DOWN);
}

static void test_acceptance_5_idle(void)
{
    /* 5. Fully ready, no control action: green steady. */
    led_state_t s = state(LED_WIFI_GOT_IP, LED_MQTT_CONNECTED, LED_SENSOR_OK, false, false, true);
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L4_IDLE);
}

static void test_acceptance_6_control_active(void)
{
    /* 6. Control action running: green short blink; L5 needs full readiness. */
    led_state_t s = state(LED_WIFI_GOT_IP, LED_MQTT_CONNECTED, LED_SENSOR_OK, true, false, true);
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L5_ACTIVE);
    s.ready = false;
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L0_INIT); /* L0 outranks L5 */
}

static void test_acceptance_7_fault(void)
{
    /* 7. Fault or safe lock: red triple blink, highest priority. */
    led_state_t s = state(LED_WIFI_GOT_IP, LED_MQTT_CONNECTED, LED_SENSOR_OK, false, true, true);
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L6_FAULT);
    s = state(LED_WIFI_GOT_IP, LED_MQTT_CONNECTED, LED_SENSOR_FAIL, false, false, true);
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L6_FAULT); /* persistent bad sensor */
    s = state(LED_WIFI_DOWN, LED_MQTT_DOWN, LED_SENSOR_FAIL, true, true, true);
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L6_FAULT); /* beats everything */
}

static void test_acceptance_8_fault_recovery(void)
{
    /* 8. Recovery: exit L6 only after the safety logic clears the fault. */
    led_state_t s = state(LED_WIFI_GOT_IP, LED_MQTT_CONNECTED, LED_SENSOR_OK, false, true, true);
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L6_FAULT);
    s.fault = false; /* confirmed recovered */
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L4_IDLE);
    s = state(LED_WIFI_GOT_IP, LED_MQTT_CONNECTED, LED_SENSOR_FAIL, false, false, true);
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L6_FAULT);
    s.sensor = LED_SENSOR_OK; /* sensor data valid again */
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L4_IDLE);
}

static void test_acceptance_9_network_recovery(void)
{
    /* 9. Network recovery follows the connection progress, then green. */
    led_state_t s = state(LED_WIFI_DOWN, LED_MQTT_DOWN, LED_SENSOR_OK, false, false, true);
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L3_NET_DOWN);
    s.wifi = LED_WIFI_CONNECTING;
    s.mqtt = LED_MQTT_DOWN;
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L3_NET_DOWN); /* session still down */
    s.mqtt = LED_MQTT_CONNECTING;
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L1_WIFI); /* Wi-Fi retry progress */
    s.wifi = LED_WIFI_GOT_IP;
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L2_MQTT); /* MQTT reconnecting */
    s.mqtt = LED_MQTT_CONNECTED;
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L4_IDLE); /* back to green steady */
}

static void test_acceptance_10_purity(void)
{
    /* 10. The engine is pure: resolve over every state combination must be
     * total and stable (no crash, in-range result, same input -> same output).
     * Hardware faults in the LED task cannot reach control logic this way. */
    for (int w = 0; w <= (int)LED_WIFI_GOT_IP; w++) {
        for (int m = 0; m <= (int)LED_MQTT_CONNECTED; m++) {
            for (int se = 0; se <= (int)LED_SENSOR_FAIL; se++) {
                for (int mask = 0; mask < 8; mask++) {
                    led_state_t s = state((led_wifi_state_t)w, (led_mqtt_state_t)m,
                                          (led_sensor_state_t)se, (mask & 1) != 0,
                                          (mask & 2) != 0, (mask & 4) != 0);
                    led_display_t d1 = led_pattern_resolve(&s);
                    led_display_t d2 = led_pattern_resolve(&s);
                    CHECK(d1 == d2);
                    CHECK((int)d1 >= 0 && (int)d1 < (int)LED_DISPLAY_COUNT);
                    const led_pattern_t *p = led_pattern_get(d1);
                    CHECK(p->n_segs >= 1 && p->n_segs <= LED_PATTERN_MAX_SEGS);
                    for (unsigned i = 0; i < p->n_segs; i++) {
                        CHECK(p->segs[i].color >= LED_COLOR_OFF && p->segs[i].color <= LED_COLOR_PURPLE);
                    }
                }
            }
        }
    }
    /* Out-of-range display index falls back safely. */
    const led_pattern_t *p = led_pattern_get((led_display_t)LED_DISPLAY_COUNT);
    CHECK(p == led_pattern_get(LED_DISPLAY_L0_INIT));
}

static void test_priority_order(void)
{
    /* Spec §4: L6 > L3 > L1 > L2 > L0 > L5 > L4. */
    led_state_t s = state(LED_WIFI_CONNECTING, LED_MQTT_CONNECTING, LED_SENSOR_OK, true, true, true);
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L6_FAULT); /* L6 over L3/L1/L5 */
    s.fault = false;
    s.wifi = LED_WIFI_DOWN;
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L3_NET_DOWN); /* L3 over L1/L2/L5 */
    s.wifi = LED_WIFI_CONNECTING;
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L1_WIFI); /* L1 over L2/L5 */
    s.wifi = LED_WIFI_GOT_IP;
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L2_MQTT); /* L2 over L0/L5 */
    s.ready = false;
    s.sensor = LED_SENSOR_UNKNOWN;
    s.mqtt = LED_MQTT_CONNECTED;
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L0_INIT); /* L0 over L5 */
    s = state(LED_WIFI_GOT_IP, LED_MQTT_CONNECTED, LED_SENSOR_OK, true, false, false);
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L0_INIT); /* L0 over L5 */
    s = state(LED_WIFI_GOT_IP, LED_MQTT_CONNECTED, LED_SENSOR_OK, true, false, true);
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L5_ACTIVE); /* L5 over L4 */
    s.control_active = false;
    CHECK(led_pattern_resolve(&s) == LED_DISPLAY_L4_IDLE);
}

static void test_no_false_fault_on_startup(void)
{
    /* Spec §1.6: startup must never show red just because the sensor is idle. */
    led_state_t s = state(LED_WIFI_INIT, LED_MQTT_INIT, LED_SENSOR_UNKNOWN, false, false, false);
    CHECK(led_pattern_resolve(&s) != LED_DISPLAY_L6_FAULT);
    s = state(LED_WIFI_CONNECTING, LED_MQTT_INIT, LED_SENSOR_UNKNOWN, false, false, false);
    CHECK(led_pattern_resolve(&s) != LED_DISPLAY_L6_FAULT);
}

static void test_color_table(void)
{
    /* Spec §5 RGB channel table. */
    bool r, g, b;
    led_color_rgb(LED_COLOR_OFF, &r, &g, &b);
    CHECK(!r && !g && !b);
    led_color_rgb(LED_COLOR_RED, &r, &g, &b);
    CHECK(r && !g && !b);
    led_color_rgb(LED_COLOR_GREEN, &r, &g, &b);
    CHECK(!r && g && !b);
    led_color_rgb(LED_COLOR_BLUE, &r, &g, &b);
    CHECK(!r && !g && b);
    led_color_rgb(LED_COLOR_YELLOW, &r, &g, &b);
    CHECK(r && g && !b);
    led_color_rgb(LED_COLOR_PURPLE, &r, &g, &b);
    CHECK(r && !g && b);
}

int main(void)
{
    test_pattern_timings();
    test_acceptance_1_startup();
    test_acceptance_2_wifi_connecting();
    test_acceptance_3_mqtt_connecting();
    test_acceptance_4_net_down();
    test_acceptance_5_idle();
    test_acceptance_6_control_active();
    test_acceptance_7_fault();
    test_acceptance_8_fault_recovery();
    test_acceptance_9_network_recovery();
    test_acceptance_10_purity();
    test_priority_order();
    test_no_false_fault_on_startup();
    test_color_table();
    TEST_REPORT("test_led_pattern");
}
