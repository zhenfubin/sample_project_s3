#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Plausible value limits used by payload validation. */
#define CTRL_LIMIT_MIN_C (-40.0f)
#define CTRL_LIMIT_MAX_C (125.0f)

/* A command is only trusted as un-expired when the device clock is at or
 * after this epoch (2020-09-13T12:26:40Z). Below that the clock is treated
 * as unsynchronised. */
#define CTRL_MIN_VALID_EPOCH (1600000000LL)

typedef enum {
    CTRL_CMD_SET_THRESHOLDS = 0,
    CTRL_CMD_MANUAL_ON,
    CTRL_CMD_MANUAL_OFF,
    CTRL_CMD_AUTO,
} ctrl_cmd_type_t;

typedef enum {
    CTRL_CMD_RES_APPLIED = 0,
    CTRL_CMD_RES_REJECTED,
    CTRL_CMD_RES_DUPLICATE,
} ctrl_cmd_result_t;

typedef struct {
    ctrl_cmd_type_t type;
    int sensor;              /* from payload, must equal the topic index */
    bool has_thresholds;
    float start_c;
    float stop_c;
    char cmd_id[48];
    bool has_expires_at;
    int64_t expires_at;      /* unix seconds */
} ctrl_cmd_t;

/* Build the unified telemetry payload:
 * {"sensor":..,"celsius":..,"heating":..,"relay_status":"..","start":..,"stop":..,"mode":".."}
 * Returns the number of bytes written, or -1 on any invalid value. */
int ctrl_payload_build(char *buf, size_t cap, int sensor, float celsius, bool heating,
                       float start_c, float stop_c, bool manual);

/* Strictly parse and validate a command payload.
 *  - json_len bounds the parse
 *  - topic_sensor is the sensor index extracted from the MQTT topic, or -1
 *    when the topic carries no index (device-level command topic)
 *  - now_unix is the current unix time in seconds; values below
 *    CTRL_MIN_VALID_EPOCH mean "clock unsynchronised"
 * On failure returns false and writes a short reason into err. */
bool ctrl_cmd_parse(const char *json, int json_len, int topic_sensor, int64_t now_unix,
                    ctrl_cmd_t *out, char *err, size_t err_cap);

/* Extract the sensor index from a topic of the form
 * .../sensors/<index>/... . Returns -1 when no valid index exists. */
int ctrl_topic_sensor_index(const char *topic);

const char *ctrl_cmd_type_str(ctrl_cmd_type_t t);
const char *ctrl_cmd_result_str(ctrl_cmd_result_t r);

#ifdef __cplusplus
}
#endif
