idf_component_register(
    SRCS "wifi_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_http_server esp_hw_support app_cfg nvs_flash event_manager led_status
)
