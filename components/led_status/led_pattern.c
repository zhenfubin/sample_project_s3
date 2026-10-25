#include "led_pattern.h"

/* Patterns per spec §2. Segment durations are on/off in sequence;
 * a single segment with dur_ms == 0 holds its color steady (L4). */
static const led_pattern_t s_patterns[LED_DISPLAY_COUNT] = {
    /* L0: blue, slow blink 200ms on / 800ms off */
    [LED_DISPLAY_L0_INIT] = { .segs = { { LED_COLOR_BLUE, 200 }, { LED_COLOR_OFF, 800 } }, .n_segs = 2 },
    /* L1: blue, fast blink 200ms on / 200ms off */
    [LED_DISPLAY_L1_WIFI] = { .segs = { { LED_COLOR_BLUE, 200 }, { LED_COLOR_OFF, 200 } }, .n_segs = 2 },
    /* L2: purple (red+blue), breathing 600ms on / 600ms off */
    [LED_DISPLAY_L2_MQTT] = { .segs = { { LED_COLOR_PURPLE, 600 }, { LED_COLOR_OFF, 600 } }, .n_segs = 2 },
    /* L3: yellow (red+green), double blink 150/150/150 then 1200ms off */
    [LED_DISPLAY_L3_NET_DOWN] = {
        .segs = { { LED_COLOR_YELLOW, 150 }, { LED_COLOR_OFF, 150 }, { LED_COLOR_YELLOW, 150 }, { LED_COLOR_OFF, 1200 } },
        .n_segs = 4,
    },
    /* L4: green, solid */
    [LED_DISPLAY_L4_IDLE] = { .segs = { { LED_COLOR_GREEN, 0 } }, .n_segs = 1 },
    /* L5: green, slow blink 150ms on / 1850ms off */
    [LED_DISPLAY_L5_ACTIVE] = { .segs = { { LED_COLOR_GREEN, 150 }, { LED_COLOR_OFF, 1850 } }, .n_segs = 2 },
    /* L6: red, triple blink 150/150/150/150/150 then 900ms off */
    [LED_DISPLAY_L6_FAULT] = {
        .segs = {
            { LED_COLOR_RED, 150 }, { LED_COLOR_OFF, 150 }, { LED_COLOR_RED, 150 },
            { LED_COLOR_OFF, 150 }, { LED_COLOR_RED, 150 }, { LED_COLOR_OFF, 900 },
        },
        .n_segs = 6,
    },
};

static bool wifi_ok(led_wifi_state_t w)
{
    return w == LED_WIFI_GOT_IP;
}

static bool mqtt_ok(led_mqtt_state_t m)
{
    return m == LED_MQTT_CONNECTED;
}

led_display_t led_pattern_resolve(const led_state_t *s)
{
    /* L6: fault / safe lock / persistent invalid sensor data. */
    if (s->fault || s->sensor == LED_SENSOR_FAIL) {
        return LED_DISPLAY_L6_FAULT;
    }
    /* L3: network down and not yet reconnecting. */
    if (s->wifi == LED_WIFI_DOWN || s->mqtt == LED_MQTT_DOWN) {
        return LED_DISPLAY_L3_NET_DOWN;
    }
    /* L1: Wi-Fi connecting (also after a line drop, while retrying). */
    if (s->wifi == LED_WIFI_CONNECTING) {
        return LED_DISPLAY_L1_WIFI;
    }
    /* L2: Wi-Fi up, MQTT connecting (or waiting). */
    if (wifi_ok(s->wifi) && !mqtt_ok(s->mqtt)) {
        return LED_DISPLAY_L2_MQTT;
    }
    /* L0: startup / not fully initialized (includes sensor UNKNOWN). */
    if (!wifi_ok(s->wifi) || !mqtt_ok(s->mqtt) || s->sensor != LED_SENSOR_OK || !s->ready) {
        return LED_DISPLAY_L0_INIT;
    }
    /* L5: control loop active (e.g. heating). */
    if (s->control_active) {
        return LED_DISPLAY_L5_ACTIVE;
    }
    /* L4: ready and idle. */
    return LED_DISPLAY_L4_IDLE;
}

const led_pattern_t *led_pattern_get(led_display_t d)
{
    if ((unsigned)d >= LED_DISPLAY_COUNT) {
        d = LED_DISPLAY_L0_INIT;
    }
    return &s_patterns[d];
}

void led_color_rgb(led_color_t c, bool *r, bool *g, bool *b)
{
    *r = (c == LED_COLOR_RED || c == LED_COLOR_YELLOW || c == LED_COLOR_PURPLE);
    *g = (c == LED_COLOR_GREEN || c == LED_COLOR_YELLOW);
    *b = (c == LED_COLOR_BLUE || c == LED_COLOR_PURPLE);
}
