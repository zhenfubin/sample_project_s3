idf_component_register(
    SRCS "device_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_event
        esp_netif
        esp_timer
        event_manager
        mqtt_manager
)
