#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sensor + control + telemetry task cluster, event-driven (spec §4).
 * Commands arrive as APP_EVENT_MQTT_MESSAGE; all publishes go through the
 * mqtt_manager pump. The relay GPIO is only written through this module. */
typedef struct {
    const char *device_id;
    int sensor_index;        /* topic sensor index, 0 for the SHT40 */
    bool start_safe_locked;  /* restart-guard lockout -> maintenance mode */
    float start_c;
    float stop_c;
} temperature_control_cfg_t;

esp_err_t temperature_control_start(const temperature_control_cfg_t *cfg);

/* Safety override used by the health monitor: forces the relay off and
 * latches SAFE_LOCKED in the control state machine. */
void temperature_control_emergency_off(void);

#ifdef __cplusplus
}
#endif
