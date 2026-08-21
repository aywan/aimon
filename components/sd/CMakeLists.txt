idf_component_register(
    SRCS "sd.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES "fatfs" "sdmmc" "esp_driver_sdmmc"
)
