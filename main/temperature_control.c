/**
 * @file temperature_control.c
 * @brief Sensor + control + telemetry cluster, event-driven (spec §4).
 *
 * APP_EVENT_MQTT_MESSAGE handler is light: topic-prefix check + queue push.
 * Parsing and control happen on the control task; every publish goes
 * through the mqtt_manager pump, so nothing here can block on the broker.
 */
#include "temperature_control.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "app_cfg.h"
#include "app_boot.h"
#include "app_health.h"
#include "ctrl_logic.h"
#include "ctrl_protocol.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "event_manager.h"
#include "device_manager.h"
#include "led_status.h"
#include "mqtt_manager.h"
#include "relay.h"
#include "sdkconfig.h"
#include "sht40.h"

static const char *TAG = "temperature_control";

/* 队列深度:采样 4 / 命令 4;命令去重缓存 16 条 */
#define SAMPLE_QUEUE_LEN 4
#define CMD_QUEUE_LEN    4
#define CMD_ID_CACHE_LEN 16

/* 采样队列消息:传感器任务 -> 控制任务 */
typedef struct {
    bool valid;     /* 本次采样是否有效(CRC/量程校验通过) */
    float celsius;  /* 温度(摄氏度),valid=false 时无意义 */
    float humidity; /* 湿度(%RH),valid=false 时无意义 */
} sample_msg_t;

/* 组件全局状态 */
static struct {
    temperature_control_cfg_t cfg;  /* 启动配置:设备 ID/阈值/安全锁 */
    ctrl_state_t st;                /* 控制状态机(ctrl_core 纯 C,可主机测试) */
    QueueHandle_t samples;          /* 采样队列(sample_msg_t) */
    QueueHandle_t cmds;             /* 命令队列(app_event_mqtt_msg_t) */
    char dev_id[32];                /* 设备 ID,如 s3-thermostat-xxxxxx */
    char topic_prefix[48];          /* 本设备主题前缀 "devices/<id>/" */
    bool mqtt_was_connected;        /* 上一轮 MQTT 连接标志(重连边沿检测) */
    char cmd_id_cache[CMD_ID_CACHE_LEN][48]; /* 命令去重 cmd_id 环形缓存 */
    unsigned cmd_id_next;           /* 去重缓存写入游标 */
} s_app;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* 主题拼装统一使用 mqtt_manager 的 mqtt_mgr_make_topic()(带截断检测) */

static void publish_telemetry(const sample_msg_t *m)
{
    char topic[MQTT_TOPIC_MAX];
    char payload[256];
    if (mqtt_mgr_make_topic(topic, sizeof(topic), "devices/%s/sensors/%d/telemetry", s_app.dev_id,
                   s_app.cfg.sensor_index) < 0) {
        return;
    }
    /* 每次采样均上报:含湿度与传感器状态;故障样本测量值为 null */
    if (ctrl_payload_build(payload, sizeof(payload), s_app.cfg.sensor_index, m->celsius,
                           m->humidity, m->valid, s_app.st.heating,
                           s_app.st.params.start_c, s_app.st.params.stop_c,
                           ctrl_mode(&s_app.st) != CTRL_MODE_AUTO) < 0) {
        ESP_LOGE(TAG, "telemetry payload build failed (invalid values), not sent");
        return;
    }
    mqtt_manager_publish(topic, payload, 0, false);
}

static void publish_state(const char *why)
{
    char topic[MQTT_TOPIC_MAX];
    char payload[384];
    wifi_ap_record_t ap;
    int rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
    }
    mqtt_mgr_make_topic(topic, sizeof(topic), "devices/%s/state", s_app.dev_id);
    snprintf(payload, sizeof(payload),
             "{\"sensor\":%d,\"celsius\":%.2f,\"heating\":%s,\"relay_status\":\"%s\","
             "\"start\":%.2f,\"stop\":%.2f,\"mode\":\"%s\",\"protection\":\"%s\","
             "\"sample_valid\":%s,\"uptime_s\":%" PRIu32 ",\"rssi\":%d,\"reason\":\"%s\"}",
             s_app.cfg.sensor_index, (double)s_app.st.celsius,
             s_app.st.heating ? "true" : "false", ctrl_relay_status_str(s_app.st.heating),
             (double)s_app.st.params.start_c, (double)s_app.st.params.stop_c,
             ctrl_mode_str(ctrl_mode(&s_app.st)), ctrl_prot_str(ctrl_prot(&s_app.st)),
             s_app.st.sensor_valid ? "true" : "false", now_ms() / 1000, rssi, why);
    mqtt_manager_publish(topic, payload, 1, true);
}

static void publish_config(void)
{
    char topic[MQTT_TOPIC_MAX];
    char payload[256];
    mqtt_mgr_make_topic(topic, sizeof(topic), "devices/%s/config", s_app.dev_id);
    snprintf(payload, sizeof(payload),
             "{\"sensor\":%d,\"start\":%.2f,\"stop\":%.2f,\"overtemp\":%.2f,"
             "\"sample_period_s\":%.1f,\"sensor_stale_s\":%.1f,\"manual_max_s\":%" PRIu32 "}",
             s_app.cfg.sensor_index, (double)s_app.st.params.start_c,
             (double)s_app.st.params.stop_c, (double)s_app.st.params.overtemp_c,
             (double)s_app.st.params.sample_period_ms / 1000.0,
             (double)s_app.st.params.sensor_stale_ms / 1000.0,
             s_app.st.params.manual_max_ms / 1000);
    mqtt_manager_publish(topic, payload, 1, true);
}

/* Idempotent MQTT-based discovery registration (retained). */
static void publish_discovery(void)
{
    char topic[MQTT_TOPIC_MAX];
    char payload[1024];
    mqtt_mgr_make_topic(topic, sizeof(topic), "devices/%s/discovery", s_app.dev_id);
    snprintf(payload, sizeof(payload),
             "{\"device_id\":\"%s\",\"firmware\":\"%s\",\"platform\":\"esp32s3\","
             "\"sensors\":[{\"index\":%d,\"driver\":\"sht40\",\"unit\":\"celsius\","
             "\"humidity_unit\":\"percent\",\"sample_period_s\":%.1f}],"
             "\"actuator\":{\"type\":\"relay\",\"gpio\":%d,"
             "\"semantics\":\"commanded_state\"},"
             "\"topics\":{\"telemetry\":\"devices/%s/sensors/%d/telemetry\","
             "\"command\":\"devices/%s/sensors/%d/command\","
             "\"command_result\":\"devices/%s/sensors/%d/command_result\","
             "\"state\":\"devices/%s/state\",\"config\":\"devices/%s/config\","
             "\"availability\":\"devices/%s/availability\"}}",
             s_app.dev_id, "1.0.0", s_app.cfg.sensor_index,
             (double)s_app.st.params.sample_period_ms / 1000.0,
#ifdef CONFIG_APP_RELAY_GPIO
             CONFIG_APP_RELAY_GPIO,
#else
             -1,
#endif
             s_app.dev_id, s_app.cfg.sensor_index, s_app.dev_id, s_app.cfg.sensor_index,
             s_app.dev_id, s_app.cfg.sensor_index, s_app.dev_id, s_app.dev_id, s_app.dev_id);
    mqtt_manager_publish(topic, payload, 1, true);
}

static void publish_cmd_result(const ctrl_cmd_t *cmd, const char *result, const char *reason)
{
    char topic[MQTT_TOPIC_MAX];
    char payload[256];
    int sensor = cmd ? cmd->sensor : s_app.cfg.sensor_index;
    mqtt_mgr_make_topic(topic, sizeof(topic), "devices/%s/sensors/%d/command_result",
                        s_app.dev_id, sensor);
    snprintf(payload, sizeof(payload),
             "{\"sensor\":%d,\"cmd_id\":\"%s\",\"type\":\"%s\",\"result\":\"%s\","
             "\"reason\":\"%s\",\"start\":%.2f,\"stop\":%.2f,\"mode\":\"%s\"}",
             sensor, cmd && cmd->cmd_id[0] ? cmd->cmd_id : "",
             cmd ? ctrl_cmd_type_str(cmd->type) : "unknown", result, reason,
             (double)s_app.st.params.start_c, (double)s_app.st.params.stop_c,
             ctrl_mode_str(ctrl_mode(&s_app.st)));
    mqtt_manager_publish(topic, payload, 1, false);
}

static bool cmd_id_seen(const char *cmd_id)
{
    if (cmd_id == NULL || cmd_id[0] == '\0') {
        return false;
    }
    for (unsigned i = 0; i < CMD_ID_CACHE_LEN; i++) {
        if (s_app.cmd_id_cache[i][0] != '\0' && strcmp(s_app.cmd_id_cache[i], cmd_id) == 0) {
            return true;
        }
    }
    snprintf(s_app.cmd_id_cache[s_app.cmd_id_next % CMD_ID_CACHE_LEN],
             sizeof(s_app.cmd_id_cache[0]), "%s", cmd_id);
    s_app.cmd_id_next++;
    return false;
}

/* Returns NULL on success, otherwise a short reason. */
static const char *apply_cmd(const ctrl_cmd_t *cmd, uint32_t t)
{
    switch (cmd->type) {
    case CTRL_CMD_SET_THRESHOLDS:
        if (!ctrl_apply_thresholds(&s_app.st, cmd->start_c, cmd->stop_c, t)) {
            return "thresholds_invalid";
        }
        if (app_cfg_save_thresholds(cmd->start_c, cmd->stop_c) != ESP_OK) {
            return "persist_failed"; /* thresholds applied but not stored */
        }
        return NULL;
    case CTRL_CMD_MANUAL_ON:
        return ctrl_manual_on(&s_app.st, t) ? NULL : "refused_by_protection";
    case CTRL_CMD_MANUAL_OFF:
        return ctrl_manual_off(&s_app.st, t) ? NULL : "failed";
    case CTRL_CMD_AUTO:
    default: {
        bool was_locked = ctrl_prot(&s_app.st) == CTRL_PROT_SAFE_LOCKED;
        ctrl_restore_auto(&s_app.st, t);
        if (was_locked && ctrl_prot(&s_app.st) != CTRL_PROT_SAFE_LOCKED) {
            /* Explicit operator unlock clears the restart-guard lockout. */
            boot_guard_reset();
            ESP_LOGW(TAG, "SAFE_LOCKED exited by operator, restart guard cleared");
        }
        return NULL;
    }
    }
}

static void process_commands(uint32_t t)
{
    app_event_mqtt_msg_t msg;
    while (xQueueReceive(s_app.cmds, &msg, 0) == pdTRUE) {
        ctrl_cmd_t cmd;
        char err[32] = {0};
        int topic_sensor = ctrl_topic_sensor_index(msg.topic);

        if (!ctrl_cmd_parse(msg.payload, msg.len, topic_sensor, device_time_unix(), &cmd, err,
                            sizeof(err))) {
            ESP_LOGW(TAG, "command rejected: %s (topic=%s)", err, msg.topic);
            publish_cmd_result(NULL, ctrl_cmd_result_str(CTRL_CMD_RES_REJECTED), err);
            continue;
        }
        if (cmd_id_seen(cmd.cmd_id)) {
            ESP_LOGI(TAG, "重复命令已跳过: cmd_id=%s", cmd.cmd_id);
            publish_cmd_result(&cmd, ctrl_cmd_result_str(CTRL_CMD_RES_DUPLICATE), "seen");
            continue;
        }
        /* Re-check expiry at apply time (queueing may have delayed us). */
        if (cmd.has_expires_at && device_time_synced() && cmd.expires_at <= device_time_unix()) {
            ESP_LOGW(TAG, "命令已过期,拒绝执行: cmd_id=%s", cmd.cmd_id);
            publish_cmd_result(&cmd, ctrl_cmd_result_str(CTRL_CMD_RES_REJECTED), "expired");
            continue;
        }
        const char *reason = apply_cmd(&cmd, t);
        publish_cmd_result(&cmd, ctrl_cmd_result_str(reason ? CTRL_CMD_RES_REJECTED
                                                           : CTRL_CMD_RES_APPLIED),
                           reason ? reason : "ok");
        if (reason == NULL) {
            ESP_LOGI(TAG, "命令已执行: %s sensor=%d cmd_id=%s", ctrl_cmd_type_str(cmd.type),
                     cmd.sensor, cmd.cmd_id);
            publish_state("command");
            publish_config();
        }
    }
}

static void apply_output(void)
{
    bool out = ctrl_output(&s_app.st);
    if (out != relay_get()) {
        relay_set(out); /* the only place the relay GPIO is written */
        ESP_LOGI(TAG, "relay -> %s", ctrl_relay_status_str(out));
        publish_state("output_change");
    }
}

/* Light handler: own-topic check + bounded queue push, nothing else. */
static void on_mqtt_message(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    const app_event_mqtt_msg_t *msg = (const app_event_mqtt_msg_t *)data;
    size_t prefix_len = strlen(s_app.topic_prefix);
    if (strncmp(msg->topic, s_app.topic_prefix, prefix_len) != 0) {
        ESP_LOGW(TAG, "foreign topic ignored: %s", msg->topic);
        return;
    }
    app_event_mqtt_msg_t copy = *msg;
    if (xQueueSend(s_app.cmds, &copy, 0) != pdTRUE) {
        ESP_LOGW(TAG, "command queue full, dropping %s", msg->topic);
    }
}

static void sensor_task(void *arg)
{
    (void)arg;
    for (;;) {
        float t = 0, rh = 0;
        esp_err_t err = sht40_read(&t, &rh);
        bool valid = (err == ESP_OK);
        /* 温度探测串口日志:每次采样输出温度/湿度与读取结果 */
        if (valid) {
            ESP_LOGI(TAG, "温湿度采样: 温度=%.2fC 湿度=%.1f%%", (double)t, (double)rh);
        } else {
            ESP_LOGW(TAG, "温湿度采样失败: %s (CRC/超时/超量程)", esp_err_to_name(err));
        }
        sample_msg_t m = {.valid = valid, .celsius = valid ? t : 0.0f,
                          .humidity = valid ? rh : 0.0f};
        if (xQueueSend(s_app.samples, &m, 0) != pdTRUE) {
            sample_msg_t dump;
            xQueueReceive(s_app.samples, &dump, 0); /* 队列满:丢弃最旧样本 */
            xQueueSend(s_app.samples, &m, 0);
        }
        app_health_note(APP_HEALTH_SENSOR);
        vTaskDelay(pdMS_TO_TICKS(s_app.st.params.sample_period_ms));
    }
}

static void control_task(void *arg)
{
    (void)arg;
    TickType_t wake = xTaskGetTickCount();
    uint32_t last_state_pub = 0;
    bool healthy_marked = false;

    for (;;) {
        uint32_t t = now_ms();

        sample_msg_t m;
        while (xQueueReceive(s_app.samples, &m, 0) == pdTRUE) {
            ctrl_on_sample(&s_app.st, m.valid, m.celsius, t);
            led_status_set_sensor_ok(m.valid);
            publish_telemetry(&m); /* 每次采样均上报遥测(含湿度与传感器状态) */
        }

        process_commands(t);
        ctrl_tick(&s_app.st, t);
        apply_output();

        /* Status LED (spec: only state pushes here; the LED task owns GPIOs). */
        ctrl_prot_t prot = ctrl_prot(&s_app.st);
        led_status_set_fault(prot != CTRL_PROT_NONE && prot != CTRL_PROT_COOLDOWN);
        led_status_set_control_active(s_app.st.heating);

        bool mqtt_up = mqtt_manager_connected();
        if (mqtt_up && !s_app.mqtt_was_connected) {
            publish_discovery();
            publish_config();
            publish_state("reconnect");
        }
        s_app.mqtt_was_connected = mqtt_up;

        if (mqtt_up && (t - last_state_pub) > 30000) {
            publish_state("periodic");
            last_state_pub = t;
        }

        if (!healthy_marked && (t / 1000) > 300) {
            boot_guard_mark_healthy();
            healthy_marked = true;
            ESP_LOGI(TAG, "5 minutes of stable uptime, restart guard counter reset");
        }

        app_health_note(APP_HEALTH_CONTROL);
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(1000));
    }
}

esp_err_t temperature_control_start(const temperature_control_cfg_t *cfg)
{
    if (cfg == NULL || cfg->device_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&s_app, 0, sizeof(s_app));
    s_app.cfg = *cfg;
    snprintf(s_app.dev_id, sizeof(s_app.dev_id), "%s", cfg->device_id);
    snprintf(s_app.topic_prefix, sizeof(s_app.topic_prefix), "devices/%s/", s_app.dev_id);

    ctrl_params_t params;
    ctrl_set_defaults(&params);
    if (ctrl_validate_thresholds(&params, cfg->start_c, cfg->stop_c)) {
        params.start_c = cfg->start_c;
        params.stop_c = cfg->stop_c;
    } else {
        ESP_LOGW(TAG, "configured thresholds invalid, using defaults 35/37");
    }
    ctrl_init(&s_app.st, &params);
    if (cfg->start_safe_locked) {
        ctrl_enter_safe_locked(&s_app.st, now_ms());
        ESP_LOGW(TAG, "started in SAFE_LOCKED (restart guard lockout)");
    }

    s_app.samples = xQueueCreate(SAMPLE_QUEUE_LEN, sizeof(sample_msg_t));
    s_app.cmds = xQueueCreate(CMD_QUEUE_LEN, sizeof(app_event_mqtt_msg_t));
    if (s_app.samples == NULL || s_app.cmds == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = event_manager_register(APP_EVENT_MQTT_MESSAGE, on_mqtt_message, NULL);
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(sensor_task, "sensor", 4096, NULL, 10, NULL) != pdPASS ||
        xTaskCreate(control_task, "control", 6144, NULL, 11, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void temperature_control_emergency_off(void)
{
    relay_set(false);
    ctrl_enter_safe_locked(&s_app.st, now_ms());
    led_status_set_fault(true);
    ESP_LOGE(TAG, "EMERGENCY: relay forced off, control latched SAFE_LOCKED");
}
