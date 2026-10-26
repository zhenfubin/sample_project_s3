idf_component_register(
    SRCS "fan_ctrl.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_ledc esp_driver_pcnt esp_driver_gpio esp_timer
)
