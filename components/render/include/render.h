#pragma once

#include "lcd.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Set up LVGL (display + touch), build the screens, and subscribe to
 *        state changes.
 *
 * Must be called after lcd_init(). Owns the LVGL lock internally — other
 * modules never need to call lvgl_port_lock()/unlock() themselves; they go
 * through state_set_*() / events_post() instead.
 */
void render_init(const lcd_handle_t *lcd);

#ifdef __cplusplus
}
#endif
