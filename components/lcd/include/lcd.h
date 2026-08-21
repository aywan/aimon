#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

// Waveshare ESP32-S3-Touch-LCD-3.49: AXS15231B (LCD + touch), QSPI, 172x640
#define LCD_H_RES       172
#define LCD_V_RES       640

typedef struct {
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t panel_handle;
    // System I2C bus (TCA9554 expander, PCF85063 RTC, IMU/audio codecs) — exposed so
    // other modules (e.g. mic) can hang their own device off the same bus
    // instead of trying to re-init I2C0 on the same pins, which the
    // underlying i2c_master driver doesn't allow.
    i2c_master_bus_handle_t sys_i2c_bus;
} lcd_handle_t;

/**
 * @brief Bring up the QSPI bus, AXS15231B panel, backlight and touch
 *        controller.
 */
void lcd_init(lcd_handle_t *out_lcd);

/**
 * @brief Set backlight brightness.
 *
 * @param percent 0-100.
 */
void lcd_backlight_set(uint8_t percent);

/**
 * @brief Turn the backlight off if it's on, or restore it to its last
 *        brightness if it's off.
 */
void lcd_backlight_toggle(void);

/**
 * @brief Release the TCA9554 power-hold latch, cutting hardware power to
 *        the whole board.
 *
 * This board only stays powered after the PWR button is released because
 * firmware asserts that latch at boot (see lcd_init()) — releasing it here
 * is what a real "power off" means on this hardware, as opposed to deep
 * sleep. The board is fully unpowered until the PWR button is physically
 * pressed again, so this call does not return.
 */
void lcd_power_off(void);

/**
 * @brief Poll the touch controller once.
 *
 * Coordinates are in the panel's native raw frame (0..LCD_V_RES-1 x
 * 0..LCD_H_RES-1) — callers that present the display rotated are
 * responsible for remapping to their own logical frame.
 *
 * @param[out] x,y      Touch point, valid only when *pressed is true.
 * @param[out] pressed  Whether a finger is currently down.
 * @return ESP_OK, or the I2C error if the read failed (treat as "not
 *         pressed" — this touch chip occasionally misses a transaction).
 */
esp_err_t lcd_touch_read(uint16_t *x, uint16_t *y, bool *pressed);

#ifdef __cplusplus
}
#endif
