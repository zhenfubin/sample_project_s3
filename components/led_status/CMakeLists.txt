idf_component_register(
    SRCS "led_status.c" "led_pattern.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_timer
        freertos
        led_strip
)
