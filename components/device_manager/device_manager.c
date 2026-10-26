/**
 * @file device_manager.c
 * @brief Device lifecycle broadcasts and SNTP time service.
 *
 * All blocking work (SNTP wait, MQTT publishes) runs on the dev_mgr task;
 * APP_EVENT handlers only set flags and notify.
 */
#include "device_manager.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "event_manager.h"
#include "mqtt_manager.h"

static const char *TAG = "device_manager";

#define MIN_TRUSTED_EPOCH 1600000000LL /* 2020-09-13: plausible wall clock */

static char s_device_id[32];
static TaskHandle_t s_task;
static volatile bool s_local_ready;
static volatile bool s_ready_posted;
static volatile bool s_need_sntp;
static volatile bool s_sntp_started;
static volatile bool s_err_pending;
static int s_err_code;
static char s_err_source[16];
static volatile bool s_fan_stall_pending; /* 风扇堵转事件待发(断线期间保留) */
static int s_fan_stall_fan;
static int s_fan_stall_duty;
static int s_fan_stall_rpm;

static void dev_mgr_task(void *arg)
{
    (void)arg;
    bool was_connected = false;
    char topic[MQTT_TOPIC_MAX];
    char payload[192];

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

        if (s_need_sntp) {
            s_need_sntp = false;
            if (!s_sntp_started) {
                esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
                cfg.start = true;
                cfg.server_from_dhcp = false;
                esp_err_t err = esp_netif_sntp_init(&cfg);
                if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                    ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
                } else {
                    s_sntp_started = true;
                }
            }
            if (s_sntp_started && !device_time_synced()) {
                /* Bounded: the device stays fully functional without sync. */
                esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "time synced: %lld", (long long)device_time_unix());
                } else {
                    ESP_LOGW(TAG, "SNTP sync pending");
                }
            }
        }

        bool connected = mqtt_manager_connected();
        if (connected && !was_connected) {
            /* Connected edge: (re)announce readiness and pending errors. */
            /* 主题拼装统一使用 mqtt_manager 的 mqtt_mgr_make_topic() */
            /* 设备事件统一发布到 devices/{id}/events,payload 的 event 字段区分类型 */
            if (s_local_ready) {
                mqtt_mgr_make_topic(topic, sizeof(topic), "devices/%s/events", s_device_id);
                snprintf(payload, sizeof(payload),
                         "{\"event\":\"device_ready\",\"device_id\":\"%s\",\"uptime_s\":%lld}",
                         s_device_id, (long long)(esp_timer_get_time() / 1000000));
                if (mqtt_manager_publish(topic, payload, 1, false) >= 0) {
                    s_ready_posted = true;
                }
            }
        }
        was_connected = connected;

        if (connected && s_err_pending) {
            s_err_pending = false;
            mqtt_mgr_make_topic(topic, sizeof(topic), "devices/%s/events", s_device_id);
            snprintf(payload, sizeof(payload),
                     "{\"event\":\"system_error\",\"device_id\":\"%s\",\"error\":%d,"
                     "\"source\":\"%s\"}",
                     s_device_id, s_err_code, s_err_source);
            /* 一次性事件,QoS1、非 retained,避免订阅者收到旧事件 */
            mqtt_manager_publish(topic, payload, 1, false);
        }

        if (connected && s_fan_stall_pending) {
            s_fan_stall_pending = false;
            mqtt_mgr_make_topic(topic, sizeof(topic), "devices/%s/events", s_device_id);
            snprintf(payload, sizeof(payload),
                     "{\"event\":\"fan_stall\",\"device_id\":\"%s\",\"fan\":%d,"
                     "\"duty\":%d,\"rpm\":%d}",
                     s_device_id, s_fan_stall_fan, s_fan_stall_duty, s_fan_stall_rpm);
            /* 堵转一次性事件:QoS1、非 retained */
            mqtt_manager_publish(topic, payload, 1, false);
        }
    }
}

static void on_app_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == APP_EVENT_WIFI_GOT_IP) {
        s_need_sntp = true;
        if (s_task) {
            xTaskNotifyGive(s_task);
        }
    }
}

esp_err_t device_manager_init(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_task != NULL) {
        return ESP_OK;
    }
    snprintf(s_device_id, sizeof(s_device_id), "%s", device_id);

    esp_err_t err = event_manager_register(APP_EVENT_WIFI_GOT_IP, on_app_event, NULL);
    if (err != ESP_OK) {
        return err;
    }
    if (xTaskCreate(dev_mgr_task, "dev_mgr", 4096, NULL, 7, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "device manager up for %s", s_device_id);
    return ESP_OK;
}

void device_manager_set_local_ready(void)
{
    if (s_local_ready) {
        return; /* idempotent: APP_EVENT_DEVICE_READY fires exactly once */
    }
    s_local_ready = true;
    event_manager_publish(APP_EVENT_DEVICE_READY, TAG, 0, NULL, 0);
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void device_manager_report_error(int err, const char *source)
{
    app_event_error_t payload = {.err = err};
    snprintf(payload.source, sizeof(payload.source), "%s", source ? source : "unknown");

    s_err_code = err;
    snprintf(s_err_source, sizeof(s_err_source), "%s", payload.source);
    s_err_pending = true;

    event_manager_publish(APP_EVENT_SYSTEM_ERROR, payload.source, err, &payload, sizeof(payload));
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void device_manager_report_fan_stall(int fan, int duty, int rpm)
{
    app_event_fan_t payload = {.fan = fan, .duty = duty, .rpm = rpm};

    s_fan_stall_fan = fan;
    s_fan_stall_duty = duty;
    s_fan_stall_rpm = rpm;
    s_fan_stall_pending = true;

    event_manager_publish(APP_EVENT_FAN_STALL, TAG, 0, &payload, sizeof(payload));
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

bool device_time_synced(void)
{
    return (int64_t)time(NULL) >= MIN_TRUSTED_EPOCH;
}

int64_t device_time_unix(void)
{
    return (int64_t)time(NULL);
}
