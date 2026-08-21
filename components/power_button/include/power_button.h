#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Watch the onboard PWR button (GPIO16, active-low).
 *
 * A short press toggles the screen backlight on/off. A long press (>=1.5s)
 * cuts hardware power to the whole board via lcd_power_off() — the board is
 * fully unpowered until the PWR button is physically pressed again, which
 * re-latches power and restarts app_main() from scratch.
 *
 * The screen turns on automatically at boot (see lcd_init()). The release of
 * whatever press powered the board on is not treated as a short press —
 * otherwise that same press would immediately toggle the just-turned-on
 * screen back off.
 */
void power_button_init(void);

#ifdef __cplusplus
}
#endif
