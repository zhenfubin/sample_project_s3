#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * wifi_manager (design spec V1.1)
 *
 * Owns Wi-Fi STA + SoftAP setup portal and Wi-Fi retry. Posts APP_EVENT
 * notifications only:
 *   WIFI_CONNECTED     STA associated (no IP yet)
 *   WIFI_GOT_IP        STA got an IP  -> MQTT start gate
 *   WIFI_DISCONNECTED  STA lost link  -> MQTT reconnect pause gate
 *
 * All publishes go through event_manager_publish() (non-blocking).
 */

#define WIFI_MGR_SSID_MAX 33
#define WIFI_MGR_PASS_MAX 65

typedef struct {
    char ssid[WIFI_MGR_SSID_MAX];
    char pass[WIFI_MGR_PASS_MAX];
} wifi_mgr_creds_t;

esp_err_t wifi_mgr_start(void);

/* Current Wi-Fi state (safe to call any time). */
bool wifi_mgr_is_connected(void);
bool wifi_mgr_got_ip(void);
bool wifi_mgr_portal_active(void);

/* Copy the current SSID (empty string when not connected). */
esp_err_t wifi_mgr_get_ssid(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
