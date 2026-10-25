idf_component_register(
    SRCS "mqtt_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_event
        esp_hw_support
        mqtt
        event_manager
        wifi_manager
        led_status
)
