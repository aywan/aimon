idf_component_register(
    SRCS "battery.c"
    INCLUDE_DIRS "include"
    REQUIRES "state"
    PRIV_REQUIRES "esp_adc" "esp_timer"
)
