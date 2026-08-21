idf_component_register(
    SRCS "ble.c" "gatt_svc.c"
    INCLUDE_DIRS "include"
    REQUIRES "state" "nvs_flash" "esp_timer" "bt" "rtc"
    PRIV_REQUIRES "cjson"
)
