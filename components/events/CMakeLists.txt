idf_component_register(
    SRCS "events.c"
    INCLUDE_DIRS "include"
    REQUIRES "state"
    PRIV_REQUIRES "ble"
)
