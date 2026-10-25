/**
 * @file led_pattern.h
 * @brief Pure-logic RGB LED state-to-pattern engine (per RGB LED status spec V1.0).
 *
 * No ESP-IDF dependencies: host-testable. Maps the aggregated system state
 * (Wi-Fi / MQTT / sensor / control / fault / ready) to exactly one display
 * state (L0..L6) with the spec priority L6>L3>L1>L2>L0>L5>L4, and provides
 * the blink pattern and R/G/B channel table for each display state.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Logical LED colors (single channel or fixed pair per spec §5). */
typedef enum {
    LED_COLOR_OFF = 0,
    LED_COLOR_RED,
    LED_COLOR_GREEN,
    LED_COLOR_BLUE,
    LED_COLOR_YELLOW, /* red + green */
    LED_COLOR_PURPLE, /* red + blue */
} led_color_t;

/** Wi-Fi state as pushed by wifi_manager. */
typedef enum {
    LED_WIFI_INIT = 0,   /* not started yet */
    LED_WIFI_CONNECTING, /* connecting / reconnecting / reassociating, no IP */
    LED_WIFI_DOWN,       /* link lost and not yet in a reconnect attempt */
    LED_WIFI_GOT_IP,     /* has IP */
} led_wifi_state_t;

/** MQTT state as pushed by mqtt_manager. */
typedef enum {
    LED_MQTT_INIT = 0,   /* not started / waiting for network */
    LED_MQTT_CONNECTING, /* connecting or reconnecting */
    LED_MQTT_DOWN,       /* established session lost, not yet reconnecting */
    LED_MQTT_CONNECTED,
} led_mqtt_state_t;

/** Sensor state. UNKNOWN until the first sample is judged (no false fault). */
typedef enum {
    LED_SENSOR_UNKNOWN = 0,
    LED_SENSOR_OK,
    LED_SENSOR_FAIL,
} led_sensor_state_t;

/** Aggregated system state fed to led_pattern_resolve(). */
typedef struct {
    led_wifi_state_t wifi;
    led_mqtt_state_t mqtt;
    led_sensor_state_t sensor;
    bool control_active; /* control loop running an output (e.g. heating) */
    bool fault;          /* safety fault or safe lock active */
    bool ready;          /* control initialized + system ready */
} led_state_t;

/** Display states per spec §2. */
typedef enum {
    LED_DISPLAY_L0_INIT = 0,
    LED_DISPLAY_L1_WIFI,
    LED_DISPLAY_L2_MQTT,
    LED_DISPLAY_L3_NET_DOWN,
    LED_DISPLAY_L4_IDLE,
    LED_DISPLAY_L5_ACTIVE,
    LED_DISPLAY_L6_FAULT,
    LED_DISPLAY_COUNT,
} led_display_t;

#define LED_PATTERN_MAX_SEGS 6

/** One pattern segment: color held for dur_ms; dur_ms == 0 means steady. */
typedef struct {
    led_color_t color;
    uint32_t dur_ms;
} led_seg_t;

/** Cyclic blink pattern: segments played in order, then repeated. */
typedef struct {
    led_seg_t segs[LED_PATTERN_MAX_SEGS];
    unsigned n_segs;
} led_pattern_t;

/**
 * @brief Resolve the single display state per the spec priority table.
 *
 * Priority: L6 (fault/safe-lock/sensor fail) > L3 (network down) >
 * L1 (Wi-Fi connecting) > L2 (Wi-Fi up, MQTT not connected) >
 * L0 (startup, system not fully ready) > L5 (control active) > L4 (idle).
 */
led_display_t led_pattern_resolve(const led_state_t *s);

/** @brief Pattern table lookup for a display state. */
const led_pattern_t *led_pattern_get(led_display_t d);

/** @brief Map a logical color to R/G/B channel levels (spec §5 table). */
void led_color_rgb(led_color_t c, bool *r, bool *g, bool *b);

#ifdef __cplusplus
}
#endif
