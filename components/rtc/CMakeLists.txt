idf_component_register(
    SRCS "rtc.c"
    INCLUDE_DIRS "include"
    REQUIRES "lcd"
    PRIV_REQUIRES "esp_driver_i2c"
)
