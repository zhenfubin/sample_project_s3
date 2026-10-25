#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Plausible value limits used by payload validation. */
#define CTRL_LIMIT_MIN_C (-40.0f)  /* 载荷校验:温度下限 */
#define CTRL_LIMIT_MAX_C (125.0f)  /* 载荷校验:温度上限 */
#define CTRL_LIMIT_MIN_RH (0.0f)   /* 载荷校验:湿度下限(%RH) */
#define CTRL_LIMIT_MAX_RH (100.0f) /* 载荷校验:湿度上限(%RH) */

/* A command is only trusted as un-expired when the device clock is at or
 * after this epoch (2020-09-13T12:26:40Z). Below that the clock is treated
 * as unsynchronised. */
#define CTRL_MIN_VALID_EPOCH (1600000000LL) /* 时钟可信的最小 Unix 时间戳 */

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
    ctrl_cmd_type_t type; /* 命令类型 */
    int sensor;              /* 载荷中的传感器编号,必须与主题编号一致 */
    bool has_thresholds;     /* 是否携带阈值字段 */
    float start_c;           /* 新的加热启动阈值 */
    float stop_c;            /* 新的加热停止阈值 */
    char cmd_id[48];         /* 命令去重 ID */
    bool has_expires_at;     /* 是否携带过期时间 */
    int64_t expires_at;      /* 过期时间(Unix 秒) */
} ctrl_cmd_t;

/* Build the unified telemetry payload:
 * {"sensor":..,"celsius":..,"humidity":..,"sensor_status":"..","heating":..,
 *  "relay_status":"..","start":..,"stop":..,"mode":".."}
 * humidity 单位 %RH,有效范围 [0, 100];sensor_ok 为假(传感器故障样本)时
 * celsius/humidity 输出 null、sensor_status 输出 "fault",测量值不参与校验。
 * Returns the number of bytes written, or -1 on any invalid value. */
int ctrl_payload_build(char *buf, size_t cap, int sensor, float celsius, float humidity_rh,
                       bool sensor_ok, bool heating, float start_c, float stop_c, bool manual);

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
