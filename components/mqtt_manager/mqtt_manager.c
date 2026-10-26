/**
 * @file mqtt_manager.c
 * @brief Single-owner MQTT module: got-IP gated start, one reconnect
 *        supervisor, one publish pump, inbound messages posted as
 *        APP_EVENT_MQTT_MESSAGE.
 *
 * Stall fix: only the pump task ever calls esp_mqtt_client_enqueue(), so the
 * esp-mqtt API lock held during a ~10 s blocking transport connect can never
 * stall an application task.
 */
#include "mqtt_manager.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_client.h"

#include "event_manager.h"
#include "led_status.h"
#include "wifi_manager.h"

static const char *TAG = "mqtt_manager";

#define PUB_QUEUE_LEN    8
#define PUB_PAYLOAD_MAX  1280 /* discovery JSON is ~1 KB */
#define BACKOFF_MIN_MS   1000
#define BACKOFF_MAX_MS   30000 /* spec §8: cap 30 s */

/* 单条待发布消息(经有界队列由 pub_task 统一发送) */
typedef struct {
    char topic[MQTT_TOPIC_MAX];     /* 目的主题 */
    char payload[PUB_PAYLOAD_MAX];  /* 载荷(JSON 文本) */
    int qos;                        /* 0/1 */
    bool retain;                    /* 保留标志 */
} pub_msg_t;

static esp_mqtt_client_handle_t s_client;   /* esp-mqtt 客户端句柄 */
static QueueHandle_t s_pub_queue;           /* 发布有界队列(仅 pub_task 出队) */
static TaskHandle_t s_supervisor;           /* 重连主管任务(唯一重连发起者) */
static volatile bool s_connected;           /* MQTT 会话已建立 */
static volatile bool s_session_up; /* an established session was dropped */
static volatile bool s_started;             /* esp-mqtt 客户端已 start */
static volatile bool s_net_ready;           /* Wi-Fi 已获 IP,允许连接 */
static volatile bool s_sup_reset;           /* 复位重连退避计数 */
static char s_device_id[32];                /* 设备 ID */
static char s_avail_topic[MQTT_TOPIC_MAX];  /* 遗嘱/在线主题 devices/{id}/availability */
static char s_cmd_topic[MQTT_TOPIC_MAX];     /* 命令主题 devices/{id}/sensors/0/command(唯一传感器命令入口) */
static char s_fan_cmd_topic[MQTT_TOPIC_MAX]; /* 风扇命令主题 devices/{id}/fans/0/command */

static void supervisor_task(void *arg);
static void pub_task(void *arg);

int mqtt_mgr_make_topic(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

static void handle_mqtt_event(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        s_session_up = true;
        led_status_set_mqtt(LED_MQTT_CONNECTED);
        ESP_LOGI(TAG, "connected to broker");
        /* Safe here: the client's own event context, recursive API lock. */
        esp_mqtt_client_publish(s_client, s_avail_topic, "online", 0, 1, 1);
        /* 仅订阅传感器/风扇命令主题(索引 0 精确主题,不用通配符);
         * 设备级 command 不支持,不订阅以防客户端误用 */
        esp_mqtt_client_subscribe(s_client, s_cmd_topic, 1);
        esp_mqtt_client_subscribe(s_client, s_fan_cmd_topic, 1);
        /* MQTT 订阅串口日志 */
        ESP_LOGI(TAG, "已订阅MQTT主题: %s", s_cmd_topic);
        ESP_LOGI(TAG, "已订阅MQTT主题: %s", s_fan_cmd_topic);
        event_manager_publish(APP_EVENT_MQTT_CONNECTED, TAG, 0, NULL, 0);
        if (s_supervisor) {
            xTaskNotifyGive(s_supervisor);
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        /* Dropped established session -> alert (L3) until the reconnect
         * starts; failed connect attempts stay in the reconnect state (L2). */
        led_status_set_mqtt(s_session_up ? LED_MQTT_DOWN : LED_MQTT_CONNECTING);
        s_session_up = false;
        ESP_LOGW(TAG, "disconnected from broker");
        event_manager_publish(APP_EVENT_MQTT_DISCONNECTED, TAG, 0, NULL, 0);
        if (s_supervisor) {
            xTaskNotifyGive(s_supervisor);
        }
        break;

    case MQTT_EVENT_DATA: {
        /* Light: copy into the event payload and hand off ownership. */
        app_event_mqtt_msg_t msg = {0};
        int tlen = ev->topic_len < MQTT_TOPIC_MAX - 1 ? ev->topic_len : MQTT_TOPIC_MAX - 1;
        memcpy(msg.topic, ev->topic, tlen);
        msg.topic[tlen] = '\0';
        int dlen = ev->data_len < MQTT_PAYLOAD_MAX - 1 ? ev->data_len : MQTT_PAYLOAD_MAX - 1;
        memcpy(msg.payload, ev->data, dlen);
        msg.payload[dlen] = '\0';
        msg.len = dlen;
        msg.retained = ev->retain;
        /* MQTT 收包串口日志:主题 + 载荷截断预览(最多 160 字符) */
        ESP_LOGI(TAG, "收到MQTT消息: topic=%s len=%d retain=%d payload=%.160s", msg.topic,
                 ev->data_len, (int)ev->retain, msg.payload);
        event_manager_publish(APP_EVENT_MQTT_MESSAGE, TAG, 0, &msg, sizeof(msg));
        break;
    }

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error (type=%d)", (int)ev->error_handle->error_type);
        break;

    default:
        break;
    }
}

static void on_app_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == APP_EVENT_WIFI_GOT_IP) {
        mqtt_manager_start();
    } else if (id == APP_EVENT_WIFI_DISCONNECTED) {
        mqtt_manager_pause();
    }
}

esp_err_t mqtt_manager_init(const char *uri, const char *device_id)
{
    if (uri == NULL || device_id == NULL || device_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_client != NULL) {
        return ESP_OK;
    }
    snprintf(s_device_id, sizeof(s_device_id), "%s", device_id);
    if (mqtt_mgr_make_topic(s_avail_topic, sizeof(s_avail_topic), "devices/%s/availability",
                            s_device_id) < 0 ||
        mqtt_mgr_make_topic(s_cmd_topic, sizeof(s_cmd_topic),
                            "devices/%s/sensors/0/command", s_device_id) < 0 ||
        mqtt_mgr_make_topic(s_fan_cmd_topic, sizeof(s_fan_cmd_topic),
                            "devices/%s/fans/0/command", s_device_id) < 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    s_pub_queue = xQueueCreate(PUB_QUEUE_LEN, sizeof(pub_msg_t));
    if (s_pub_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .credentials.client_id = s_device_id,
        .session.last_will =
            {
                .topic = s_avail_topic,
                .msg = "offline",
                .msg_len = 7,
                .qos = 1,
                .retain = true,
            },
        .network.reconnect_timeout_ms = 0, /* we drive reconnects ourselves */
        .network.disable_auto_reconnect = true,
        .task.stack_size = 6144,
        .buffer.size = 1024,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, handle_mqtt_event, NULL);

    if (xTaskCreate(pub_task, "mqtt_pub", 4096, NULL, 9, NULL) != pdPASS) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(supervisor_task, "mqtt_sup", 3072, NULL, 8, &s_supervisor) != pdPASS) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = event_manager_register(APP_EVENT_WIFI_GOT_IP, on_app_event, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = event_manager_register(APP_EVENT_WIFI_DISCONNECTED, on_app_event, NULL);
    if (err != ESP_OK) {
        return err;
    }

    /* Race guard: Wi-Fi may already hold an IP if we were re-initialised. */
    if (wifi_mgr_got_ip()) {
        return mqtt_manager_start();
    }
    ESP_LOGI(TAG, "client created; waiting for WIFI_GOT_IP");
    return ESP_OK;
}

esp_err_t mqtt_manager_start(void)
{
    if (s_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_net_ready = true;
    led_status_set_mqtt(LED_MQTT_CONNECTING);
    if (!s_started) {
        esp_err_t err = esp_mqtt_client_start(s_client);
        if (err != ESP_OK) {
            s_net_ready = false;
            return err;
        }
        s_started = true;
        ESP_LOGI(TAG, "client started");
    }
    s_sup_reset = true;
    if (s_supervisor) {
        xTaskNotifyGive(s_supervisor);
    }
    return ESP_OK;
}

void mqtt_manager_pause(void)
{
    s_net_ready = false;
    ESP_LOGI(TAG, "network lost; reconnect paused");
}

bool mqtt_manager_connected(void)
{
    return s_connected;
}

int mqtt_manager_publish(const char *topic, const char *payload, int qos, bool retain)
{
    if (topic == NULL || payload == NULL || qos < 0 || qos > 2) {
        return -1;
    }
    size_t tlen = strlen(topic);
    size_t plen = strlen(payload);
    if (tlen == 0 || tlen >= MQTT_TOPIC_MAX || plen >= PUB_PAYLOAD_MAX) {
        ESP_LOGW(TAG, "publish rejected (topic %u B, payload %u B)", (unsigned)tlen,
                 (unsigned)plen);
        return -1;
    }
    if (s_pub_queue == NULL || !s_connected) {
        return -1; /* offline: callers republish on the connected edge */
    }
    pub_msg_t msg = {0};
    memcpy(msg.topic, topic, tlen);
    memcpy(msg.payload, payload, plen);
    msg.qos = qos;
    msg.retain = retain;
    if (xQueueSend(s_pub_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "publish queue full, dropping %s", topic);
        return -1;
    }
    return (int)plen;
}

static void pub_task(void *arg)
{
    (void)arg;
    pub_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_pub_queue, &msg, pdMS_TO_TICKS(250)) != pdTRUE) {
            continue;
        }
        if (!s_connected || s_client == NULL) {
            ESP_LOGD(TAG, "drop publish to %s while offline", msg.topic);
            continue;
        }
        int rc = esp_mqtt_client_enqueue(s_client, msg.topic, msg.payload,
                                         (int)strlen(msg.payload), msg.qos, msg.retain, true);
        if (rc < 0) {
            ESP_LOGW(TAG, "enqueue failed (%d): %s", rc, msg.topic);
        } else {
            /* MQTT 发包串口日志:主题 + 载荷截断预览(最多 160 字符) */
            ESP_LOGI(TAG, "发送MQTT消息: topic=%s qos=%d retain=%d payload=%.160s", msg.topic,
                     msg.qos, (int)msg.retain, msg.payload);
        }
    }
}

static void supervisor_task(void *arg)
{
    (void)arg;
    unsigned attempt = 0;
    unsigned wait_s = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        if (s_sup_reset) {
            s_sup_reset = false;
            attempt = 0;
            wait_s = 0;
        }
        if (!s_started || !s_net_ready || s_client == NULL) {
            continue;
        }
        if (s_connected) {
            attempt = 0;
            wait_s = 0;
            continue;
        }
        if (wait_s > 0) {
            wait_s--;
            continue;
        }
        if (attempt > 8) {
            attempt = 8;
        }
        uint32_t base = BACKOFF_MIN_MS << attempt;
        if (base > BACKOFF_MAX_MS) {
            base = BACKOFF_MAX_MS;
        }
        uint32_t wait_ms = base + (esp_random() % (base / 4 + 1));
        ESP_LOGI(TAG, "reconnect attempt %u (next delay >=%u s)", attempt + 1,
                 (unsigned)(wait_ms / 1000));
        led_status_set_mqtt(LED_MQTT_CONNECTING);
        esp_mqtt_client_reconnect(s_client);
        wait_s = (wait_ms / 1000) + 1;
        attempt++;
    }
}
