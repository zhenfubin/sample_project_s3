#pragma once
/**
 * @file device_manager.h
 * @brief Device-level lifecycle events: readiness, system errors, time sync.
 *
 * Responsibilities (per event spec §4):
 *  - APP_EVENT_DEVICE_READY: emitted exactly once via
 *    device_manager_set_local_ready() when local control is up (idempotent).
 *  - APP_EVENT_SYSTEM_ERROR: posted by device_manager_report_error() and
 *    broadcast on the unified `devices/<id>/events` topic as
 *    {"event":"system_error",...} (QoS1, non-retained).
 *  - SNTP time sync after APP_EVENT_WIFI_GOT_IP, executed on the service
 *    task with a bounded wait (never on the event loop).
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Create the service task and APP_EVENT handlers. Idempotent. */
esp_err_t device_manager_init(const char *device_id);

/**
 * @brief Declare local control up; posts APP_EVENT_DEVICE_READY once.
 *
 * Further calls are no-ops. The MQTT broadcast is sent when connected.
 */
void device_manager_set_local_ready(void);

/**
 * @brief Post APP_EVENT_SYSTEM_ERROR and queue a retained broadcast.
 *
 * @param err     module error code (or esp_err_t)
 * @param source  short module name, e.g. "sensor" (max 15 chars)
 */
void device_manager_report_error(int err, const char *source);

/** @brief True once SNTP has delivered a plausible wall-clock time. */
bool device_time_synced(void);

/** @brief Seconds since Unix epoch (0 when never synced). */
int64_t device_time_unix(void);

#ifdef __cplusplus
}
#endif
