#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_APP_DEVICE_NAME "aw_extramon_1"
#define BLE_APP_PAIRING_WINDOW_SEC 60

/**
 * @brief Bring up NimBLE: advertising, pairing (passkey shown on the LCD),
 *        and the JSON control GATT service.
 *
 * New pairing is accepted only after the user taps Pair on Settings, and
 * only for BLE_APP_PAIRING_WINDOW_SEC after that tap. Already-bonded peers
 * may reconnect at any time. Restored NVS bonds are published to
 * state_set_ble_paired() from the host-sync callback — not from this call,
 * which returns before the store is readable.
 */
void ble_app_init(void);

/**
 * @brief Whether new pairing / other pairing-window-gated operations
 *        (e.g. OTA) are currently allowed.
 */
bool ble_app_pairing_window_open(void);

/**
 * @brief Start a pairing session: generate a passkey, show it via state,
 *        and accept a new bond for BLE_APP_PAIRING_WINDOW_SEC.
 *
 * No-op if a bond already exists. Posted from Settings via events, not
 * called from render directly.
 */
void ble_app_begin_pairing(void);

/**
 * @brief Drop every bonded peer (and the live link, if any) and clear
 *        state_get_ble_paired().
 *
 * The peer still has its own LTK — macOS must Forget the device separately,
 * same as after `make erase-nvs`.
 */
void ble_app_forget_paired(void);

/**
 * @brief Record that a GATT control write arrived.
 *
 * Resets the 75s silence watchdog and clears state_get_ble_data_stale().
 * Connect/encrypt do not count — only a JSON control payload.
 */
void ble_app_on_control_write(void);

#ifdef __cplusplus
}
#endif
