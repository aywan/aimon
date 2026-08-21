idf_component_register(
    SRCS "power_button.c"
    INCLUDE_DIRS "include"
    REQUIRES "lcd"
    PRIV_REQUIRES "esp_driver_gpio" "esp_hw_support" "esp_timer"
)
