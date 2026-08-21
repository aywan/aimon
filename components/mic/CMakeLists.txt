idf_component_register(
    SRCS "mic.c"
    INCLUDE_DIRS "include"
    REQUIRES "lcd"
    PRIV_REQUIRES "esp_driver_i2c" "esp_driver_i2s"
)
