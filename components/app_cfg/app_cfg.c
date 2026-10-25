#include "app_cfg.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ctrl_protocol.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "app_cfg";
static const char *NS = "app_cfg";

#define DEFAULT_START_C 35.0f
#define DEFAULT_STOP_C  37.0f

esp_err_t app_cfg_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Only recoverable NVS corruption justifies an erase. */
        ESP_LOGW(TAG, "NVS needs recovery (%s), erasing and re-initialising", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            return err;
        }
        err = nvs_flash_init();
    }
    return err;
}

static bool valid_ssid(const char *s)
{
    size_t n = strlen(s);
    return n > 0 && n < APP_CFG_SSID_MAX;
}

static bool valid_pass(const char *s)
{
    /* Open networks use an empty password; WPA2 PSK is 8..63 chars. */
    size_t n = strlen(s);
    return n < APP_CFG_PASS_MAX && (n == 0 || (n >= 8 && n <= 63));
}

static bool valid_uri(const char *s)
{
    return strncmp(s, "mqtt://", 7) == 0 && strlen(s) < APP_CFG_URI_MAX;
}

esp_err_t app_cfg_load(app_cfg_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Defaults first; each stored field is validated individually. */
    memset(out, 0, sizeof(*out));
    snprintf(out->mqtt_uri, sizeof(out->mqtt_uri), "%s", CONFIG_APP_MQTT_URI_DEFAULT);
    out->start_c = DEFAULT_START_C;
    out->stop_c = DEFAULT_STOP_C;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no config stored yet, using defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    char buf[APP_CFG_URI_MAX];
    size_t len = sizeof(buf);
    if (nvs_get_str(h, "wifi_ssid", buf, &len) == ESP_OK && valid_ssid(buf)) {
        strlcpy(out->wifi_ssid, buf, sizeof(out->wifi_ssid));
    }
    len = sizeof(buf);
    if (nvs_get_str(h, "wifi_pass", buf, &len) == ESP_OK && valid_pass(buf)) {
        strlcpy(out->wifi_pass, buf, sizeof(out->wifi_pass));
    }
    len = sizeof(buf);
    if (nvs_get_str(h, "mqtt_uri", buf, &len) == ESP_OK && valid_uri(buf)) {
        strlcpy(out->mqtt_uri, buf, sizeof(out->mqtt_uri));
    }

    float f;
    if (nvs_get_blob(h, "start_c", &f, &(size_t){sizeof(f)}) == ESP_OK &&
        isfinite(f) && f >= CTRL_LIMIT_MIN_C && f <= CTRL_LIMIT_MAX_C) {
        out->start_c = f;
    }
    if (nvs_get_blob(h, "stop_c", &f, &(size_t){sizeof(f)}) == ESP_OK &&
        isfinite(f) && f >= CTRL_LIMIT_MIN_C && f <= CTRL_LIMIT_MAX_C) {
        out->stop_c = f;
    }

    /* Cross-field rule: start < stop, stop <= overtemp default. */
    if (!(out->start_c < out->stop_c) || out->stop_c > 40.0f) {
        ESP_LOGW(TAG, "stored thresholds invalid, reverting to defaults");
        out->start_c = DEFAULT_START_C;
        out->stop_c = DEFAULT_STOP_C;
    }

    nvs_close(h);
    return ESP_OK;
}

esp_err_t app_cfg_save_wifi(const char *ssid, const char *pass)
{
    if (ssid == NULL || pass == NULL || !valid_ssid(ssid) || !valid_pass(pass)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(h, "wifi_ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "wifi_pass", pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    /* Write-verify: read the credentials straight back. */
    if (err == ESP_OK) {
        char check[APP_CFG_PASS_MAX];
        size_t len = sizeof(check);
        if (nvs_get_str(h, "wifi_ssid", check, &len) != ESP_OK || strcmp(check, ssid) != 0) {
            err = ESP_ERR_INVALID_CRC;
        }
    }

    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi credentials stored and verified");
    } else {
        ESP_LOGE(TAG, "storing Wi-Fi credentials failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t app_cfg_save_thresholds(float start_c, float stop_c)
{
    if (!isfinite(start_c) || !isfinite(stop_c) ||
        start_c < CTRL_LIMIT_MIN_C || start_c > CTRL_LIMIT_MAX_C ||
        stop_c < CTRL_LIMIT_MIN_C || stop_c > CTRL_LIMIT_MAX_C ||
        !(start_c < stop_c) || stop_c > 40.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "start_c", &start_c, sizeof(start_c));
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "stop_c", &stop_c, sizeof(stop_c));
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool app_cfg_has_wifi(const app_cfg_t *cfg)
{
    return cfg->wifi_ssid[0] != '\0';
}

esp_err_t app_cfg_save_mqtt_uri(const char *uri)
{
    if (uri == NULL || !valid_uri(uri)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "mqtt_uri", uri);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
