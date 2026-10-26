#include <stdio.h>
#include <string.h>

#include "app_boot.h"
#include "app_cfg.h"
#include "app_health.h"
#include "device_manager.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "event_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_status.h"
#include "mqtt_manager.h"
#include "relay.h"
#include "sdkconfig.h"
#include "sht40.h"
#include "temperature_control.h"
#include "wifi_manager.h"

static const char *TAG = "app_main";

static void build_device_id(char *out, size_t cap)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, cap, "%s-%02X%02X%02X", CONFIG_APP_DEVICE_ID_PREFIX, mac[3], mac[4], mac[5]);
}

void app_main(void)
{
    boot_log_reset_reason();
    guard_decision_t guard = boot_guard_check();

    /* ---- BOOT_SAFE: the relay is forced to its safe level first. ---- */
    boot_set_state(BOOT_STATE_SAFE);
    ESP_ERROR_CHECK(relay_init());
    ESP_LOGI(TAG, "relay GPIO %d initialised to SAFE level (off)", CONFIG_APP_RELAY_GPIO);

    /* ---- INIT_NVS + event loop (handlers register before any event) ---- */
    boot_set_state(BOOT_STATE_INIT_NVS);
    ESP_ERROR_CHECK(app_cfg_init());
    ESP_ERROR_CHECK(event_manager_init());
    event_manager_publish(APP_EVENT_DEVICE_STARTING, "app_main", 0, NULL, 0);

    /* ---- LOAD_CONFIG ---- */
    boot_set_state(BOOT_STATE_LOAD_CONFIG);
    app_cfg_t cfg;
    ESP_ERROR_CHECK(app_cfg_load(&cfg));
    ESP_LOGI(TAG, "config: ssid='%s' mqtt='%s' thresholds=%.1f/%.1f", cfg.wifi_ssid,
             cfg.mqtt_uri, (double)cfg.start_c, (double)cfg.stop_c);

    char device_id[32];
    build_device_id(device_id, sizeof(device_id));
    ESP_ERROR_CHECK(device_manager_init(device_id));

    /* ---- INIT_DRIVERS ---- */
    boot_set_state(BOOT_STATE_INIT_DRIVERS);
    sht40_config_t scfg = {
        .sda_gpio = CONFIG_APP_I2C_SDA_GPIO,
        .scl_gpio = CONFIG_APP_I2C_SCL_GPIO,
        .i2c_timeout_ms = 100,
    };
    if (sht40_init(&scfg) != ESP_OK) {
        /* Non-fatal: the control loop latches FAULT_SENSOR and stays safe. */
        ESP_LOGE(TAG, "SHT40 init failed, continuing in sensor-fault mode");
        device_manager_report_error(ESP_ERR_INVALID_STATE, "sensor");
    }

    /* ---- INIT_CONTROL (sensor + control + safety) ---- */
    boot_set_state(BOOT_STATE_INIT_CONTROL);
    temperature_control_cfg_t ccfg = {
        .device_id = device_id,
        .sensor_index = 0,
        .start_safe_locked = (guard == GUARD_SAFE_LOCKED),
        .start_c = cfg.start_c,
        .stop_c = cfg.stop_c,
        .fan_pwm_gpio = CONFIG_APP_FAN_PWM_GPIO,
        .fan_tach_gpio = CONFIG_APP_FAN_TACH_GPIO,
        .fan_default_duty = CONFIG_APP_FAN_DEFAULT_DUTY,
        .fan_index = 0,
    };
    ESP_ERROR_CHECK(temperature_control_start(&ccfg));
    app_health_start(guard == GUARD_SAFE_LOCKED);

    /* ---- INIT_LED ---- */
    boot_set_state(BOOT_STATE_INIT_LED);
    ESP_ERROR_CHECK(led_status_init());

    /* ---- INIT_WIFI ---- */
    boot_set_state(BOOT_STATE_INIT_WIFI);
    if (!app_cfg_has_wifi(&cfg)) {
        ESP_LOGW(TAG, "no Wi-Fi credentials, provisioning portal will start");
    }
    ESP_ERROR_CHECK(wifi_mgr_start());

    /* ---- DISCOVERY (published idempotently on MQTT connect) ---- */
    boot_set_state(BOOT_STATE_DISCOVERY);

    /* ---- INIT_MQTT: create the client, do NOT connect (spec §9.7). ---- */
    boot_set_state(BOOT_STATE_INIT_MQTT);
    ESP_LOGI(TAG, "device_id=%s broker=%s", device_id, cfg.mqtt_uri);
    ESP_ERROR_CHECK(mqtt_manager_init(cfg.mqtt_uri, device_id));

    /* ---- DEVICE_READY once local control is up (spec §9.9) ---- */
    device_manager_set_local_ready();

    /* ---- SYNC_STATE: bounded wait for the network. ---- */
    boot_set_state(BOOT_STATE_SYNC_STATE);
    for (int i = 0; i < 10 && !wifi_mgr_got_ip(); i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!wifi_mgr_got_ip()) {
        ESP_LOGW(TAG, "no network yet: running fully local (DEGRADED)");
        boot_set_state(BOOT_STATE_DEGRADED);
    }

    /* ---- RUNNING / SAFE_LOCKED ---- */
    boot_set_state(guard == GUARD_SAFE_LOCKED ? BOOT_STATE_SAFE_LOCKED : BOOT_STATE_RUNNING);
    led_status_set_ready(true);
    ESP_LOGI(TAG, "boot complete: %s", boot_state_str(boot_get_state()));
}
