idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES "lcd" "render" "power_button" "ble" "battery" "mic" "rtc"
)
