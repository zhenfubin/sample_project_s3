#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Unified application event layer (single custom esp_event loop "APP_EVENT").
 *
 * Rules (design spec V1.1):
 *  - every application event goes through event_manager_publish()
 *  - publish() never blocks the caller (zero timeout) and never retries
 *  - every publish is recorded in a 32-entry in-RAM history ring
 *  - handler data pointers are valid only inside the handler
 */
ESP_EVENT_DECLARE_BASE(APP_EVENT);

typedef enum {
    APP_EVENT_DEVICE_STARTING = 0,
    APP_EVENT_WIFI_CONNECTED,
    APP_EVENT_WIFI_GOT_IP,
    APP_EVENT_WIFI_DISCONNECTED,
    APP_EVENT_MQTT_CONNECTED,
    APP_EVENT_MQTT_DISCONNECTED,
    APP_EVENT_MQTT_MESSAGE,
    APP_EVENT_DEVICE_READY,
    APP_EVENT_SYSTEM_ERROR,
    APP_EVENT_ID_MAX,
} app_event_id_t;

#define APP_EVENT_MQTT_TOPIC_MAX   160
#define APP_EVENT_MQTT_PAYLOAD_MAX 512

/* Payload of APP_EVENT_MQTT_MESSAGE. The event queue owns a copy; copy it
 * again if it must outlive the handler. */
typedef struct {
    char topic[APP_EVENT_MQTT_TOPIC_MAX];
    char payload[APP_EVENT_MQTT_PAYLOAD_MAX];
    int len;
    bool retained;
} app_event_mqtt_msg_t;

/* Payload of APP_EVENT_SYSTEM_ERROR. */
typedef struct {
    int err;
    char source[16];
} app_event_error_t;

typedef struct {
    int64_t t_ms;     /* esp_timer ms since boot */
    int32_t event_id; /* app_event_id_t */
    char source[12];  /* publishing module, for diagnostics */
    int32_t err;      /* module error code or esp_err_t */
} app_event_history_entry_t;

#define APP_EVENT_HISTORY_LEN 32

/* Create the single APP_EVENT custom loop. Idempotent. */
esp_err_t event_manager_init(void);

/* Unified publish entry: records the event in the history ring, then posts a
 * copy to the APP_EVENT loop with a zero timeout. Returns the
 * esp_event_post() result; failures are logged and counted, never retried
 * and never block the caller. */
esp_err_t event_manager_publish(app_event_id_t id, const char *source, int err,
                                const void *data, size_t data_size);

/* Register a handler on the APP_EVENT loop. */
esp_err_t event_manager_register(app_event_id_t id, esp_event_handler_t handler, void *arg);

/* Copy up to max most-recent history entries into out (newest first).
 * Returns the number of entries copied. */
size_t event_manager_history_read(app_event_history_entry_t *out, size_t max);

/* Number of publishes that failed to enter the event loop. */
uint32_t event_manager_dropped(void);

#ifdef __cplusplus
}
#endif
