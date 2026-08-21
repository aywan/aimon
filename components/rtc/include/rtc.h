#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lcd.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Probe the onboard PCF85063 (I2C 0x51 on lcd's system bus) and, if
 *        the chip's oscillator-stop flag is clear, load its calendar into
 *        the ESP32 system clock.
 *
 * Best-effort: a missing/NAK'd chip logs a warning and leaves
 * rtc_clock_get_hms() returning false until a BLE time write arrives. Time
 * writes still update the ESP32 clock even if the chip is absent, so the
 * UI clock works until the next power-off.
 *
 * Must be called after lcd_init() (shares I2C0). Does not abort boot.
 */
void rtc_clock_init(const lcd_handle_t *lcd);

/**
 * @brief Wall-clock from the computer: UTC unix seconds plus the computer's
 *        timezone offset in seconds east of UTC.
 *
 * Local civil time (unix + tz_offset) is what gets written to the PCF85063
 * and what rtc_clock_get_hms() returns. The ESP32 system clock is set to that
 * same local time treated as UTC, so time() ticks without further I2C.
 */
esp_err_t rtc_clock_set_unix(int64_t unix_utc, int32_t tz_offset_sec);

/**
 * @brief Current local hours/minutes/seconds, or false if time has never
 *        been set (chip OS flag was set at boot and no BLE write since).
 */
bool rtc_clock_get_hms(uint8_t *hour, uint8_t *minute, uint8_t *second);

#ifdef __cplusplus
}
#endif
