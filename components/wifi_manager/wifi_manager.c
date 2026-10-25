#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "app_cfg.h"
#include "event_manager.h"
#include "led_status.h"
#include "wifi_manager.h"

static const char *TAG = "wifi_manager";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define FAILS_BEFORE_PORTAL 8

/*
 * Retry backoff 1 -> 2 -> 4 -> 8 -> 16 -> 30 s (capped) + random jitter.
 * Every association/IP change posts APP_EVENT notifications; the cap keeps
 * behaviour inside the event spec and its tests.
 */
#define BACKOFF_MIN_MS 1000   /* 重连退避起始 1s */
#define BACKOFF_MAX_MS 30000  /* 退避上限 30s(协议 §8) */

#define PORTAL_SSID_PREFIX "THERM-SETUP-" /* 配网热点名称前缀 */
#define PORTAL_PASS "setup-1234"          /* 配网热点初始密码 */
#define PORTAL_BIND_PORT 80               /* 配网页 HTTP 端口 */

#define WIFI_TASK_STACK 4096
#define WIFI_TASK_PRIO 9

typedef enum {
    WIFI_STATE_IDLE = 0,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_PORTAL,
} wifi_state_t;

static EventGroupHandle_t s_wifi_events;
static wifi_state_t s_state = WIFI_STATE_IDLE;  /* Wi-Fi 连接状态机 */
static volatile bool s_associated = false;      /* 已关联 AP */
static volatile bool s_got_ip = false;          /* 已获取 IP */
static bool s_portal_active = false;            /* 配网热点已开启 */
static esp_netif_t *s_ap_netif = NULL;          /* 配网 AP 网络接口 */
static httpd_handle_t s_httpd = NULL;           /* 配网 HTTP 服务句柄 */
static TaskHandle_t s_wifi_task = NULL;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data);
static void wifi_task(void *arg);
static void start_portal(void);
static esp_err_t portal_get_handler(httpd_req_t *req);
static esp_err_t portal_save_handler(httpd_req_t *req);

/* 生成配网热点名称:THERM-SETUP-XXXX(XXXX = STA MAC 后两字节大写十六进制) */
static void build_portal_name(char *out, size_t cap)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, cap, "%s%02X%02X", PORTAL_SSID_PREFIX, mac[4], mac[5]);
}

esp_err_t wifi_mgr_start(void)
{
    if (s_wifi_events == NULL) {
        s_wifi_events = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_wifi_events != NULL, ESP_ERR_NO_MEM, TAG, "event group alloc failed");
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "default event loop create failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage set failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL),
                        TAG, "wifi handler register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL),
                        TAG, "ip handler register failed");

    if (xTaskCreate(wifi_task, "wifi_mgr", WIFI_TASK_STACK, NULL, WIFI_TASK_PRIO, &s_wifi_task) != pdPASS) {
        ESP_LOGE(TAG, "wifi task creation failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool wifi_mgr_is_connected(void)
{
    return s_associated;
}

bool wifi_mgr_got_ip(void)
{
    return s_got_ip;
}

bool wifi_mgr_portal_active(void)
{
    return s_portal_active;
}

esp_err_t wifi_mgr_get_ssid(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (!s_associated) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        strlcpy(out, (const char *)ap.ssid, cap);
    }
    return ESP_OK;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA started, connecting");
        led_status_set_wifi(LED_WIFI_CONNECTING);
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t *d = (wifi_event_sta_connected_t *)data;
        s_associated = true;
        s_state = WIFI_STATE_CONNECTING;
        ESP_LOGI(TAG, "AP associated (%s)", d ? (const char *)d->ssid : "?");
        event_manager_publish(APP_EVENT_WIFI_CONNECTED, "wifi_manager", 0, NULL, 0);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        int reason = d ? d->reason : 0;
        bool had_link = s_associated || s_got_ip;
        s_associated = false;
        s_got_ip = false;
        s_state = WIFI_STATE_CONNECTING;
        /* Dropped link -> alert (L3) until the next reconnect attempt (L1);
         * failed association attempts keep the connecting state (L1). */
        led_status_set_wifi(had_link ? LED_WIFI_DOWN : LED_WIFI_CONNECTING);
        ESP_LOGI(TAG, "AP lost (reason=%d), recovering", reason);
        event_manager_publish(APP_EVENT_WIFI_DISCONNECTED, "wifi_manager", reason, NULL, 0);
        if (s_wifi_events != NULL) {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        s_associated = true;
        s_got_ip = true;
        s_state = WIFI_STATE_CONNECTING;
        led_status_set_wifi(LED_WIFI_GOT_IP);
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&event->ip_info.ip));
        event_manager_publish(APP_EVENT_WIFI_GOT_IP, "wifi_manager", 0, NULL, 0);
        if (s_wifi_events != NULL) {
            xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        }
    }
}

static void wifi_task(void *arg)
{
    (void)arg;
    char portal_name[32];
    build_portal_name(portal_name, sizeof(portal_name));

    wifi_mgr_creds_t creds = {0};
    uint8_t attempt = 0;
    app_cfg_t cfg_store;
    bool have_creds = false;
    if (app_cfg_load(&cfg_store) == ESP_OK && app_cfg_has_wifi(&cfg_store)) {
        strlcpy(creds.ssid, cfg_store.wifi_ssid, sizeof(creds.ssid));
        strlcpy(creds.pass, cfg_store.wifi_pass, sizeof(creds.pass));
        have_creds = true;
    }

    if (!have_creds) {
        ESP_LOGW(TAG, "No Wi-Fi credentials saved; starting setup portal (%s)", portal_name);
        s_state = WIFI_STATE_PORTAL;
        start_portal();
    } else {
        wifi_config_t cfg = {0};
        strlcpy((char *)cfg.sta.ssid, creds.ssid, sizeof(cfg.sta.ssid));
        strlcpy((char *)cfg.sta.password, creds.pass, sizeof(cfg.sta.password));
        cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
        cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;

        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &cfg);
        s_state = WIFI_STATE_CONNECTING;
        esp_wifi_start();
    }

    while (1) {
        xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                            pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));

        if (s_portal_active) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (s_got_ip) {
            attempt = 0;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        attempt++;
        if (attempt >= FAILS_BEFORE_PORTAL) {
            if (!s_portal_active) {
                ESP_LOGW(TAG, "Wi-Fi attempts exceeded limit (%d), starting setup portal (%s)",
                         FAILS_BEFORE_PORTAL, portal_name);
                s_state = WIFI_STATE_PORTAL;
                start_portal();
            }
            continue;
        }

        uint32_t wait = BACKOFF_MIN_MS << (attempt > 5 ? 5 : attempt);
        if (wait > BACKOFF_MAX_MS) wait = BACKOFF_MAX_MS;
        wait += esp_random() % (wait / 4 + 1);
        ESP_LOGI(TAG, "Wi-Fi attempt %d failed; retry in %u ms", attempt, (unsigned)wait);
        vTaskDelay(pdMS_TO_TICKS(wait));
        led_status_set_wifi(LED_WIFI_CONNECTING);
        esp_wifi_connect();
    }
}

static void start_portal(void)
{
    if (s_portal_active) return;

    /* Awaiting provisioning: network unavailable -> L3 network-down alert. */
    led_status_set_wifi(LED_WIFI_DOWN);

    esp_netif_t *ap = s_ap_netif;
    if (ap == NULL) {
        ap = s_ap_netif = esp_netif_create_default_wifi_ap();
    }
    wifi_config_t cfg = {0};

    char portal_name[32];
    build_portal_name(portal_name, sizeof(portal_name));
    strlcpy((char *)cfg.ap.ssid, portal_name, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = strlen((char *)cfg.ap.ssid);
    strlcpy((char *)cfg.ap.password, PORTAL_PASS, sizeof(cfg.ap.password));
    cfg.ap.max_connection = 4;
    cfg.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    esp_netif_ip_info_t ip_info = {
        .ip.addr = ESP_IP4TOADDR(192, 168, 4, 1),
        .netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0),
        .gw.addr = ESP_IP4TOADDR(192, 168, 4, 1),
    };
    esp_netif_dhcps_stop(ap);
    esp_netif_set_ip_info(ap, &ip_info);
    esp_netif_dhcps_start(ap);

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &cfg);
    esp_wifi_start();

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.server_port = PORTAL_BIND_PORT;
    http_cfg.stack_size = 6144;
    if (httpd_start(&s_httpd, &http_cfg) == ESP_OK) {
        httpd_uri_t uri_get = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = portal_get_handler,
            .user_ctx = NULL
        };
        httpd_uri_t uri_post = {
            .uri = "/save",
            .method = HTTP_POST,
            .handler = portal_save_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_httpd, &uri_get);
        httpd_register_uri_handler(s_httpd, &uri_post);
    }

    s_portal_active = true;
    ESP_LOGW(TAG, "SoftAP setup portal started: SSID=%s PASS=%s IP=192.168.4.1",
             cfg.ap.ssid, PORTAL_PASS);
}

static esp_err_t portal_get_handler(httpd_req_t *req)
{
    const char *html =
        "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Thermostat Setup</title></head><body style='font-family:sans-serif;padding:24px'>"
        "<h2>Configure Wi-Fi</h2>"
        "<form method='post' action='/save'>"
        "<label>SSID</label><br><input name='ssid' maxlength='32'><br><br>"
        "<label>Password</label><br><input name='pass' type='password' maxlength='64'><br><br>"
        "<button type='submit'>Save</button></form></body></html>";
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t portal_save_handler(httpd_req_t *req)
{
    char buf[192];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    buf[received] = '\0';

    char ssid[WIFI_MGR_SSID_MAX] = {0};
    char pass[WIFI_MGR_PASS_MAX] = {0};
    char *p = strstr(buf, "ssid=");
    if (p) {
        p += 5;
        char *end = strchr(p, '&');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len >= sizeof(ssid)) len = sizeof(ssid) - 1;
        memcpy(ssid, p, len);
    }
    p = strstr(buf, "pass=");
    if (p) {
        p += 5;
        size_t len = strlen(p);
        if (len >= sizeof(pass)) len = sizeof(pass) - 1;
        memcpy(pass, p, len);
    }

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
        return ESP_FAIL;
    }

    if (app_cfg_save_wifi(ssid, pass) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }

    const char *ok = "<!DOCTYPE html><html><body style='font-family:sans-serif;padding:24px'>"
                     "<h2>Saved. Rebooting...</h2></body></html>";
    httpd_resp_send(req, ok, HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}
