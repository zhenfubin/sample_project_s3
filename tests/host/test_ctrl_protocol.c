/* Host unit tests for the MQTT payload protocol (strict validation). */

#include "ctrl_protocol.h"
#include "test_util.h"

#define NOW 1700000000LL /* clock is synchronised */

static bool parse_ok(const char *json, int topic_sensor, ctrl_cmd_t *cmd)
{
    char err[64] = {0};
    bool ok = ctrl_cmd_parse(json, (int)strlen(json), topic_sensor, NOW, cmd, err, sizeof(err));
    if (!ok) {
        printf("  (rejected: %s)\n", err);
    }
    return ok;
}

static bool parse_fails(const char *json, int topic_sensor, const char *want_reason)
{
    ctrl_cmd_t cmd;
    char err[64] = {0};
    bool ok = ctrl_cmd_parse(json, (int)strlen(json), topic_sensor, NOW, &cmd, err, sizeof(err));
    CHECK(!ok);
    CHECK_EQ_STR(err, want_reason);
    return !ok;
}

static bool parse_rejected(const char *json, int topic_sensor)
{
    ctrl_cmd_t cmd;
    char err[64] = {0};
    bool ok = ctrl_cmd_parse(json, (int)strlen(json), topic_sensor, NOW, &cmd, err, sizeof(err));
    CHECK(!ok);
    return !ok;
}

static void test_payload_build(void)
{
    char buf[256];
    int n = ctrl_payload_build(buf, sizeof(buf), 0, 36.25f, true, 35.0f, 37.0f, false);
    CHECK(n > 0);
    CHECK(strstr(buf, "\"sensor\":0") != NULL);
    CHECK(strstr(buf, "\"celsius\":36.25") != NULL);
    CHECK(strstr(buf, "\"heating\":true") != NULL);
    CHECK(strstr(buf, "\"relay_status\":\"heating\"") != NULL);
    CHECK(strstr(buf, "\"start\":35.00") != NULL);
    CHECK(strstr(buf, "\"stop\":37.00") != NULL);
    CHECK(strstr(buf, "\"mode\":\"auto\"") != NULL);

    /* Invalid values must be refused rather than published. */
    CHECK(ctrl_payload_build(buf, sizeof(buf), 0, NAN, false, 35, 37, false) < 0);
    CHECK(ctrl_payload_build(buf, sizeof(buf), 0, INFINITY, false, 35, 37, false) < 0);
    CHECK(ctrl_payload_build(buf, sizeof(buf), -1, 36.0f, false, 35, 37, false) < 0);
    CHECK(ctrl_payload_build(buf, sizeof(buf), 32, 36.0f, false, 35, 37, false) < 0);
    CHECK(ctrl_payload_build(buf, sizeof(buf), 0, 300.0f, false, 35, 37, false) < 0);
    CHECK(ctrl_payload_build(buf, 4, 0, 36.0f, false, 35, 37, false) < 0); /* tiny buffer */
}

static void test_topic_index(void)
{
    CHECK(ctrl_topic_sensor_index("devices/d1/sensors/0/command") == 0);
    CHECK(ctrl_topic_sensor_index("devices/d1/sensors/3/command") == 3);
    CHECK(ctrl_topic_sensor_index("devices/d1/sensors/01/command") == 1);
    CHECK(ctrl_topic_sensor_index("devices/d1/command") == -1);
    CHECK(ctrl_topic_sensor_index("devices/d1/sensors//command") == -1);
    CHECK(ctrl_topic_sensor_index("devices/d1/sensors/3x/command") == -1);
    CHECK(ctrl_topic_sensor_index(NULL) == -1);
}

static void test_valid_commands(void)
{
    ctrl_cmd_t cmd;

    CHECK(parse_ok("{\"sensor\":0,\"start\":30.0,\"stop\":32.0,\"mode\":\"auto\"}", 0, &cmd));
    CHECK(cmd.type == CTRL_CMD_AUTO);
    CHECK(cmd.has_thresholds && cmd.start_c == 30.0f && cmd.stop_c == 32.0f);

    CHECK(parse_ok("{\"sensor\":0,\"mode\":\"manual\",\"heating\":false}", 0, &cmd));
    CHECK(cmd.type == CTRL_CMD_MANUAL_OFF);

    char json[160];
    snprintf(json, sizeof(json),
             "{\"sensor\":0,\"mode\":\"manual\",\"heating\":true,\"expires_at\":%lld}",
             (long long)(NOW + 600));
    CHECK(parse_ok(json, 0, &cmd));
    CHECK(cmd.type == CTRL_CMD_MANUAL_ON);
    CHECK(cmd.has_expires_at);

    /* Thresholds-only update keeps the current mode. */
    CHECK(parse_ok("{\"sensor\":0,\"start\":34.0,\"stop\":36.5}", 0, &cmd));
    CHECK(cmd.type == CTRL_CMD_SET_THRESHOLDS);

    /* Payload may omit the topic index context (device-level topic). */
    CHECK(parse_ok("{\"sensor\":2,\"mode\":\"manual\",\"heating\":false}", -1, &cmd));
}

static void test_sensor_mismatch(void)
{
    /* Rule 3: payload "sensor" must match the trailing topic index. */
    parse_fails("{\"sensor\":1,\"mode\":\"manual\",\"heating\":false}", 0,
                "sensor_topic_mismatch");
}

static void test_strict_types(void)
{
    /* Booleans are not numbers. */
    parse_fails("{\"sensor\":true,\"mode\":\"manual\",\"heating\":false}", 0, "bad_sensor");
    parse_fails("{\"sensor\":0,\"start\":true,\"stop\":37.0}", 0, "bad_thresholds");
    parse_fails("{\"sensor\":0,\"mode\":\"manual\",\"heating\":\"true\"}", 0, "bad_heating");
    parse_fails("{\"sensor\":0,\"mode\":1,\"heating\":false}", 0, "bad_mode");
    parse_fails("{\"sensor\":0,\"mode\":\"manual\",\"heating\":false,\"expires_at\":\"x\"}", 0,
                "bad_expires_at");

    /* Non-finite / out-of-range numbers (overflow may surface either as a
     * rejected number or as malformed JSON depending on cJSON). */
    parse_rejected("{\"sensor\":0,\"start\":1e999,\"stop\":38.0}", 0);
    parse_rejected("{\"sensor\":0,\"celsius\":1e999,\"mode\":\"manual\",\"heating\":false}", 0);
    parse_fails("{\"sensor\":0,\"start\":-500,\"stop\":38.0}", 0, "threshold_out_of_range");

    /* Malformed JSON and non-objects. */
    parse_fails("not json", 0, "malformed_json");
    parse_fails("[1,2,3]", 0, "not_object");
    parse_fails("{}", 0, "bad_sensor");

    /* Unknown fields are rejected (schema is closed). */
    parse_fails("{\"sensor\":0,\"mode\":\"manual\",\"heating\":false,\"hack\":1}", 0,
                "unknown_field");
}

static void test_threshold_rules(void)
{
    /* start and stop must appear together. */
    parse_fails("{\"sensor\":0,\"start\":35.0,\"mode\":\"auto\"}", 0, "thresholds_incomplete");
    parse_fails("{\"sensor\":0,\"stop\":37.0,\"mode\":\"auto\"}", 0, "thresholds_incomplete");
    /* start must be strictly below stop. */
    parse_fails("{\"sensor\":0,\"start\":37.0,\"stop\":35.0,\"mode\":\"auto\"}", 0,
                "start_not_below_stop");
    parse_fails("{\"sensor\":0,\"start\":36.0,\"stop\":36.0,\"mode\":\"auto\"}", 0,
                "start_not_below_stop");
}

static void test_heating_relay_consistency(void)
{
    parse_fails("{\"sensor\":0,\"heating\":true,\"relay_status\":\"off\",\"mode\":\"manual\"}", 0,
                "heating_relay_mismatch");
    parse_fails("{\"sensor\":0,\"heating\":false,\"relay_status\":\"heating\",\"mode\":\"manual\"}",
                0, "heating_relay_mismatch");
    /* heating only makes sense with manual mode. */
    parse_fails("{\"sensor\":0,\"heating\":true,\"mode\":\"auto\"}", 0,
                "heating_requires_manual_mode");
}

static void test_expiry_rules(void)
{
    /* Manual heating without expiry is refused. */
    parse_fails("{\"sensor\":0,\"mode\":\"manual\",\"heating\":true}", 0, "missing_expires_at");

    /* Expired retained manual heating command must be rejected. */
    char json[160];
    snprintf(json, sizeof(json),
             "{\"sensor\":0,\"mode\":\"manual\",\"heating\":true,\"expires_at\":%lld}",
             (long long)(NOW - 10));
    parse_fails(json, 0, "expired");

    /* Expiry too far in the future is refused. */
    snprintf(json, sizeof(json),
             "{\"sensor\":0,\"mode\":\"manual\",\"heating\":true,\"expires_at\":%lld}",
             (long long)(NOW + 48 * 3600));
    parse_fails(json, 0, "expires_too_far");

    /* With an unsynchronised clock, expiry cannot be verified -> refuse. */
    ctrl_cmd_t cmd;
    char err[64] = {0};
    snprintf(json, sizeof(json),
             "{\"sensor\":0,\"mode\":\"manual\",\"heating\":true,\"expires_at\":%lld}",
             (long long)(NOW + 600));
    bool ok = ctrl_cmd_parse(json, (int)strlen(json), 0, 100 /* pre-2020 clock */, &cmd, err,
                             sizeof(err));
    CHECK(!ok);
    CHECK_EQ_STR(err, "clock_unsynced");
}

static void test_no_action(void)
{
    /* A command must carry a recognisable action. */
    parse_fails("{\"sensor\":0}", 0, "no_action");
}

int main(void)
{
    test_payload_build();
    test_topic_index();
    test_valid_commands();
    test_sensor_mismatch();
    test_strict_types();
    test_threshold_rules();
    test_heating_relay_consistency();
    test_expiry_rules();
    test_no_action();
    TEST_REPORT("ctrl_protocol");
}
