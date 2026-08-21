#pragma once

#include <stdint.h>

#include "lcd.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring up the ES7210 mic codec (I2C, on lcd's system bus) and start
 *        an I2S capture task that continuously estimates ambient noise.
 *
 * Best-effort: if the codec doesn't ACK on I2C or I2S init fails, this logs
 * a warning and leaves mic_get_noise_level() returning 0 rather than
 * aborting boot — the noise reading is a cosmetic extra, not core function.
 */
void mic_init(const lcd_handle_t *lcd);

/**
 * @brief Approximate, uncalibrated ambient noise level, 0-100.
 *
 * A cheap RMS-over-recent-samples heuristic scaled by a fixed, unverified
 * divisor — not a real dB(SPL) measurement, and not smoothed against clipping
 * or DC offset beyond what the codec's own high-pass filter removes. Good
 * enough for a live "is it loud in here" graph, nothing more.
 */
uint8_t mic_get_noise_level(void);

#ifdef __cplusplus
}
#endif
