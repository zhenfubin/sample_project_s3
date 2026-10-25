#include "ctrl_protocol.h"

#include "ctrl_logic.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

static bool err(char *buf, size_t cap, const char *msg)
{
    if (buf && cap > 0) {
        snprintf(buf, cap, "%s", msg);
    }
    return false;
}

static bool json_num(const cJSON *o, const char *key, double *out)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (it == NULL) {
        return false;
    }
    if (!cJSON_IsNumber(it)) { /* booleans, strings and null are rejected */
        return false;
    }
    if (!isfinite(it->valuedouble)) {
        return false;
    }
    *out = it->valuedouble;
    return true;
}

static bool json_int_in(const cJSON *o, const char *key, long lo, long hi, long *out)
{
    double d;
    if (!json_num(o, key, &d)) {
        return false;
    }
    if (d != floor(d) || d < (double)lo || d > (double)hi) {
        return false;
    }
    *out = (long)d;
    return true;
}

int ctrl_payload_build(char *buf, size_t cap, int sensor, float celsius, float humidity_rh,
                       bool sensor_ok, bool heating, float start_c, float stop_c, bool manual)
{
    if (buf == NULL || cap == 0) {
        return -1;
    }
    if (sensor < 0 || sensor > 31) {
        return -1;
    }
    if (!isfinite(start_c) || !isfinite(stop_c)) {
        return -1;
    }
    /* 仅有效样本校验测量值;故障样本测量值不参与校验(输出 null) */
    if (sensor_ok) {
        if (!isfinite(celsius) || !isfinite(humidity_rh)) {
            return -1;
        }
        if (celsius < CTRL_LIMIT_MIN_C || celsius > CTRL_LIMIT_MAX_C) {
            return -1;
        }
        if (humidity_rh < CTRL_LIMIT_MIN_RH || humidity_rh > CTRL_LIMIT_MAX_RH) {
            return -1;
        }
    }

    /* 故障样本不携带测量值:输出 null 而非伪造数字 */
    char celsius_str[16];
    char humidity_str[16];
    if (sensor_ok) {
        snprintf(celsius_str, sizeof(celsius_str), "%.2f", (double)celsius);
        snprintf(humidity_str, sizeof(humidity_str), "%.2f", (double)humidity_rh);
    } else {
        snprintf(celsius_str, sizeof(celsius_str), "null");
        snprintf(humidity_str, sizeof(humidity_str), "null");
    }

    int n = snprintf(buf, cap,
                     "{\"sensor\":%d,\"celsius\":%s,\"humidity\":%s,"
                     "\"sensor_status\":\"%s\",\"heating\":%s,\"relay_status\":\"%s\","
                     "\"start\":%.2f,\"stop\":%.2f,\"mode\":\"%s\"}",
                     sensor, celsius_str, humidity_str, sensor_ok ? "ok" : "fault",
                     heating ? "true" : "false",
                     ctrl_relay_status_str(heating), (double)start_c, (double)stop_c,
                     manual ? "manual" : "auto");
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    return n;
}

int ctrl_topic_sensor_index(const char *topic)
{
    if (topic == NULL) {
        return -1;
    }
    const char *p = strstr(topic, "/sensors/");
    if (p == NULL) {
        return -1;
    }
    p += strlen("/sensors/");
    if (*p < '0' || *p > '9') {
        return -1;
    }
    long v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        if (v > 31) {
            return -1;
        }
        p++;
    }
    /* Must be a complete path segment: "0/telemetry", not "01x". */
    if (*p != '\0' && *p != '/') {
        return -1;
    }
    return (int)v;
}

/* Reject any key outside the documented protocol. */
static bool only_known_keys(const cJSON *o, const char *const *keys, size_t nkeys,
                            char *errbuf, size_t err_cap)
{
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, o) {
        if (it->string == NULL) {
            return err(errbuf, err_cap, "bad_object");
        }
        bool known = false;
        for (size_t i = 0; i < nkeys; i++) {
            if (strcmp(it->string, keys[i]) == 0) {
                known = true;
                break;
            }
        }
        if (!known) {
            return err(errbuf, err_cap, "unknown_field");
        }
    }
    return true;
}

bool ctrl_cmd_parse(const char *json, int json_len, int topic_sensor, int64_t now_unix,
                    ctrl_cmd_t *out, char *errbuf, size_t err_cap)
{
    static const char *const keys[] = {
        "sensor", "celsius", "heating", "relay_status", "start", "stop",
        "mode", "expires_at", "cmd_id",
    };

    if (out == NULL || json == NULL || json_len <= 0) {
        return err(errbuf, err_cap, "empty_payload");
    }

    cJSON *root = cJSON_ParseWithLength(json, (size_t)json_len);
    if (root == NULL) {
        return err(errbuf, err_cap, "malformed_json");
    }

    bool ok = false;
    memset(out, 0, sizeof(*out));
    out->sensor = -1;

    do {
        if (!cJSON_IsObject(root)) {
            err(errbuf, err_cap, "not_object");
            break;
        }
        if (!only_known_keys(root, keys, sizeof(keys) / sizeof(keys[0]), errbuf, err_cap)) {
            break;
        }

        long sensor = -1;
        if (!json_int_in(root, "sensor", 0, 31, &sensor)) {
            err(errbuf, err_cap, "bad_sensor");
            break;
        }
        /* Rule: payload "sensor" must match the trailing topic index. */
        if (topic_sensor >= 0 && (long)topic_sensor != sensor) {
            err(errbuf, err_cap, "sensor_topic_mismatch");
            break;
        }
        out->sensor = (int)sensor;

        const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
        const cJSON *heating = cJSON_GetObjectItemCaseSensitive(root, "heating");
        const cJSON *relay = cJSON_GetObjectItemCaseSensitive(root, "relay_status");
        const cJSON *celsius = cJSON_GetObjectItemCaseSensitive(root, "celsius");

        if (mode != NULL && !cJSON_IsString(mode)) {
            err(errbuf, err_cap, "bad_mode");
            break;
        }
        if (heating != NULL && !cJSON_IsBool(heating)) {
            err(errbuf, err_cap, "bad_heating");
            break;
        }
        if (relay != NULL && !cJSON_IsString(relay)) {
            err(errbuf, err_cap, "bad_relay_status");
            break;
        }
        if (celsius != NULL) {
            double d;
            if (!json_num(root, "celsius", &d) || d < CTRL_LIMIT_MIN_C || d > CTRL_LIMIT_MAX_C) {
                err(errbuf, err_cap, "bad_celsius");
                break;
            }
        }

        /* heating <-> relay_status consistency (telemetry rule also
         * applied to commands so a conflict can never be interpreted). */
        if (heating != NULL && relay != NULL) {
            const char *want = cJSON_IsTrue(heating) ? "heating" : "off";
            if (strcmp(relay->valuestring, want) != 0) {
                err(errbuf, err_cap, "heating_relay_mismatch");
                break;
            }
        }

        bool has_start = cJSON_GetObjectItemCaseSensitive(root, "start") != NULL;
        bool has_stop = cJSON_GetObjectItemCaseSensitive(root, "stop") != NULL;
        if (has_start != has_stop) {
            err(errbuf, err_cap, "thresholds_incomplete");
            break;
        }
        if (has_start) {
            double ds, de;
            if (!json_num(root, "start", &ds) || !json_num(root, "stop", &de)) {
                err(errbuf, err_cap, "bad_thresholds");
                break;
            }
            if (ds < CTRL_LIMIT_MIN_C || ds > CTRL_LIMIT_MAX_C ||
                de < CTRL_LIMIT_MIN_C || de > CTRL_LIMIT_MAX_C) {
                err(errbuf, err_cap, "threshold_out_of_range");
                break;
            }
            if (!(ds < de)) {
                err(errbuf, err_cap, "start_not_below_stop");
                break;
            }
            out->has_thresholds = true;
            out->start_c = (float)ds;
            out->stop_c = (float)de;
        }

        out->has_expires_at = cJSON_GetObjectItemCaseSensitive(root, "expires_at") != NULL;
        if (out->has_expires_at) {
            double ex;
            if (!json_num(root, "expires_at", &ex) || ex != floor(ex)) {
                err(errbuf, err_cap, "bad_expires_at");
                break;
            }
            out->expires_at = (int64_t)ex;
        }

        const cJSON *cid = cJSON_GetObjectItemCaseSensitive(root, "cmd_id");
        if (cid != NULL) {
            if (!cJSON_IsString(cid) || cid->valuestring == NULL ||
                strlen(cid->valuestring) >= sizeof(out->cmd_id)) {
                err(errbuf, err_cap, "bad_cmd_id");
                break;
            }
            snprintf(out->cmd_id, sizeof(out->cmd_id), "%s", cid->valuestring);
        }

        /* Determine the command action. */
        bool mode_manual = (mode != NULL && strcmp(mode->valuestring, "manual") == 0);
        bool mode_auto = (mode != NULL && strcmp(mode->valuestring, "auto") == 0);
        if (mode != NULL && !mode_manual && !mode_auto) {
            err(errbuf, err_cap, "bad_mode");
            break;
        }
        if (heating != NULL && !mode_manual) {
            err(errbuf, err_cap, "heating_requires_manual_mode");
            break;
        }

        if (mode_manual && heating != NULL && cJSON_IsTrue(heating)) {
            out->type = CTRL_CMD_MANUAL_ON;
            /* Manual heating must always carry a verifiable expiry so a
             * stale retained command can never turn heating on. */
            if (!out->has_expires_at) {
                err(errbuf, err_cap, "missing_expires_at");
                break;
            }
            if (now_unix < CTRL_MIN_VALID_EPOCH) {
                err(errbuf, err_cap, "clock_unsynced");
                break;
            }
            if (out->expires_at <= now_unix) {
                err(errbuf, err_cap, "expired");
                break;
            }
            if (out->expires_at > now_unix + 24 * 3600) {
                err(errbuf, err_cap, "expires_too_far");
                break;
            }
        } else if (mode_manual && heating != NULL && cJSON_IsFalse(heating)) {
            out->type = CTRL_CMD_MANUAL_OFF;
        } else if (mode_auto) {
            out->type = CTRL_CMD_AUTO;
        } else if (out->has_thresholds) {
            out->type = CTRL_CMD_SET_THRESHOLDS;
        } else {
            err(errbuf, err_cap, "no_action");
            break;
        }

        /* Generic expiry rule for retained / delayed commands. */
        if (out->has_expires_at && out->type != CTRL_CMD_MANUAL_ON) {
            if (now_unix < CTRL_MIN_VALID_EPOCH) {
                err(errbuf, err_cap, "clock_unsynced");
                break;
            }
            if (out->expires_at <= now_unix) {
                err(errbuf, err_cap, "expired");
                break;
            }
        }

        ok = true;
    } while (0);

    cJSON_Delete(root);
    return ok;
}

const char *ctrl_cmd_type_str(ctrl_cmd_type_t t)
{
    switch (t) {
    case CTRL_CMD_SET_THRESHOLDS:
        return "set_thresholds";
    case CTRL_CMD_MANUAL_ON:
        return "manual_on";
    case CTRL_CMD_MANUAL_OFF:
        return "manual_off";
    case CTRL_CMD_AUTO:
    default:
        return "restore_auto";
    }
}

const char *ctrl_cmd_result_str(ctrl_cmd_result_t r)
{
    switch (r) {
    case CTRL_CMD_RES_APPLIED:
        return "applied";
    case CTRL_CMD_RES_DUPLICATE:
        return "duplicate";
    case CTRL_CMD_RES_REJECTED:
    default:
        return "rejected";
    }
}
