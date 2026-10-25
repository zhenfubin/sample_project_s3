#pragma once
/**
 * @file mqtt_manager.h
 * @brief MQTT access for the APP_EVENT architecture.
 *
 * Responsibilities (per event spec §4):
 *  - Owns the esp-mqtt client: init (no connect), start (gated on
 *    APP_EVENT_WIFI_GOT_IP), pause (APP_EVENT_WIFI_DISCONNECTED).
 *  - Single reconnect owner: one supervisor task with jittered backoff
 *    (1..30 s). The esp-mqtt auto-reconnect is disabled.
 *  - All publishing goes through a bounded queue drained by one pump task,
 *    so no application task ever blocks on the esp-mqtt API lock while a
 *    transport connect is in flight (fixes the 5 s CONTROL stalls).
 *  - Inbound messages are posted to the event loop as APP_EVENT_MQTT_MESSAGE
 *    and owned by the handler; this module does not interpret payloads.
 */

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "event_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 主题/载荷上限的唯一定义在 event_manager.h(协议 §12:160/512),
 * 这里保留旧名作别名,保证事件载荷与发布缓冲尺寸永远一致 */
#define MQTT_TOPIC_MAX   APP_EVENT_MQTT_TOPIC_MAX
#define MQTT_PAYLOAD_MAX APP_EVENT_MQTT_PAYLOAD_MAX

/**
 * @brief Create the client and tasks; does NOT connect.
 *
 * Registers APP_EVENT_WIFI_GOT_IP / APP_EVENT_WIFI_DISCONNECTED handlers.
 * If an IP is already present the client starts immediately (race guard).
 * Idempotent.
 */
esp_err_t mqtt_manager_init(const char *uri, const char *device_id);

/**
 * @brief Allow the connection (first call starts the client).
 *
 * Invoked on APP_EVENT_WIFI_GOT_IP. Idempotent.
 */
esp_err_t mqtt_manager_start(void);

/**
 * @brief Pause reconnecting after Wi-Fi loss.
 *
 * Invoked on APP_EVENT_WIFI_DISCONNECTED. Light: never blocks.
 */
void mqtt_manager_pause(void);

/**
 * @brief Non-blocking publish request.
 *
 * Queues the message for the pump task; only the pump may block on the
 * esp-mqtt API lock. Returns payload length on acceptance, -1 when the
 * queue is full, disconnected, or arguments are invalid. Queued
 * publications are dropped while offline (state is republished on the
 * connected edge).
 */
int mqtt_manager_publish(const char *topic, const char *payload, int qos, bool retain);

/** @brief True while the MQTT session is established. */
bool mqtt_manager_connected(void);

/** @brief snprintf-style topic builder; -1 on truncation/error. */
int mqtt_mgr_make_topic(char *buf, size_t cap, const char *fmt, ...);

#ifdef __cplusplus
}
#endif
