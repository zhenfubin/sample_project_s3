#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 字段长度上限 */
#define APP_CFG_SSID_MAX 32  /* Wi-Fi SSID */
#define APP_CFG_PASS_MAX 64  /* Wi-Fi 密码 */
#define APP_CFG_URI_MAX  128 /* MQTT broker URI */

typedef struct {
    char wifi_ssid[APP_CFG_SSID_MAX]; /* Wi-Fi 名称(空 = 未配置) */
    char wifi_pass[APP_CFG_PASS_MAX]; /* Wi-Fi 密码(开放网络为空) */
    char mqtt_uri[APP_CFG_URI_MAX];   /* MQTT broker 地址,mqtt://... */
    float start_c;                    /* 加热启动阈值(℃) */
    float stop_c;                     /* 加热停止阈值(℃) */
} app_cfg_t;

/*
 * Configuration storage (NVS).
 *  - NVS is only erased on the two recoverable NVS corruption errors
 *    (ESP_ERR_NVS_NO_FREE_PAGES / ESP_ERR_NVS_NEW_VERSION_FOUND); it is
 *    never erased unconditionally.
 *  - Every field is validated on load; invalid fields fall back to defaults
 *    individually without discarding the rest of the config.
 */
esp_err_t app_cfg_init(void);

/* Load config, applying per-field validation and defaults. */
esp_err_t app_cfg_load(app_cfg_t *out);

/* Save Wi-Fi credentials with write + read-back verification. */
esp_err_t app_cfg_save_wifi(const char *ssid, const char *pass);

/* Save thresholds (caller must validate first). */
esp_err_t app_cfg_save_thresholds(float start_c, float stop_c);

/* Save the MQTT broker URI (validated: must be mqtt://...). */
esp_err_t app_cfg_save_mqtt_uri(const char *uri);

/* True when Wi-Fi credentials are present (non-empty SSID). */
bool app_cfg_has_wifi(const app_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
