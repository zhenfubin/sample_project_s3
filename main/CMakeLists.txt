idf_component_register(SRCS "main.c" "app_boot.c" "temperature_control.c" "app_health.c"
                    INCLUDE_DIRS "."
                    REQUIRES app_cfg relay sensor_sht40 ctrl_core wifi_manager mqtt_manager
                             device_manager led_status event_manager
                             esp_wifi esp_hw_support esp_timer nvs_flash esp_system
                    PRIV_REQUIRES esp_driver_gpio)
