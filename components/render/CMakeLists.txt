idf_component_register(
    SRCS "render.c"
    INCLUDE_DIRS "include"
    REQUIRES "lcd" "state" "events" "esp_timer" "esp_lvgl_port" "lvgl" "esp_system" "esp_driver_tsens" "mic" "rtc" "sd"
    PRIV_REQUIRES "esp_mm"
)

# UTC build stamp for the status screen. Generated into the component's
# binary dir (not the source tree) and refreshed on every build — not just
# cmake reconfigure — so two flashes of the same tree still get distinct
# timestamps. See docs/KNOWLEDGE_BASE.md (render).
set(BUILD_INFO_H ${CMAKE_CURRENT_BINARY_DIR}/build_info.h)

execute_process(
    COMMAND ${CMAKE_COMMAND} -DOUT=${BUILD_INFO_H} -P ${CMAKE_CURRENT_SOURCE_DIR}/gen_build_info.cmake
)

add_custom_target(render_build_info
    COMMAND ${CMAKE_COMMAND} -DOUT=${BUILD_INFO_H} -P ${CMAKE_CURRENT_SOURCE_DIR}/gen_build_info.cmake
    BYPRODUCTS ${BUILD_INFO_H}
    COMMENT "Generating UTC build timestamp"
    VERBATIM
)

idf_component_get_property(render_lib ${COMPONENT_NAME} COMPONENT_LIB)
add_dependencies(${render_lib} render_build_info)
target_include_directories(${render_lib} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
set_property(SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/render.c APPEND PROPERTY OBJECT_DEPENDS ${BUILD_INFO_H})

