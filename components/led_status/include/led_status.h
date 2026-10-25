/**
 * @file led_status.h
 * @brief RGB status LED driver (ESP32-S3 thermostat status specification V1.0).
 *
 * Design rules (spec §6):
 *  - Other modules (Wi-Fi / MQTT / sensor / control) only update state through
 *    these setters; only the LED task touches the LED hardware.
 *  - Exactly one display state (L0..L6) is shown at a time (spec §4).
 *  - With APP_LED_WS2812_GPIO = -1 the whole module is a no-op.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "led_pattern.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure the WS2812 RGB LED and start the LED task.
 *
 * Enabled only when APP_LED_WS2812_GPIO >= 0 (ESP32-S3-DevKitC-1: GPIO48,
 * onboard WS2812); otherwise the module stays a no-op. Safe to call once
 * from app_main.
 */
esp_err_t led_status_init(void);

/** Wi-Fi state: INIT / CONNECTING / DOWN / GOT_IP (L0/L1/L3 gating). */
void led_status_set_wifi(led_wifi_state_t state);

/** MQTT state: INIT / CONNECTING / DOWN / CONNECTED (L2/L3 gating). */
void led_status_set_mqtt(led_mqtt_state_t state);

/**
 * Sensor sample validity. Only the judged result of real samples may be
 * pushed: before the first sample the internal state stays UNKNOWN so that
 * startup never falsely reports a fault (spec §4 principle 6).
 */
void led_status_set_sensor_ok(bool ok);

/** True while the control loop is actively driving an output (L5). */
void led_status_set_control_active(bool active);

/** True while a safety fault or safe lock is active (L6). */
void led_status_set_fault(bool fault);

/** True once control is initialized and the system is ready (L4/L5 gate). */
void led_status_set_ready(bool ready);

#ifdef __cplusplus
}
#endif
