#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_SCREEN_MAIN,
    APP_SCREEN_STATUS,
    APP_SCREEN_SETTINGS,
    APP_SCREEN_COUNT,
} app_screen_t;

#define APP_SLOT_COUNT      4
#define APP_SLOT_LABEL_MAX  16
#define APP_SLOT_TEXT_MAX   32

typedef enum {
    APP_SLOT_EMPTY = 0,
    APP_SLOT_GAUGE,
    APP_SLOT_TEXT,
} app_slot_type_t;

/**
 * One cell of the main-screen 4-slot row. Strings are fixed buffers (no
 * pointers) so getters can copy the whole struct without sharing storage
 * with the writer.
 */
typedef struct {
    app_slot_type_t type;
    uint8_t value; /* 0-100, gauge only */
    bool fg_set;
    bool bg_set;
    uint32_t fg; /* 0xRRGGBB: gauge indicator or text body; used when fg_set */
    uint32_t bg; /* 0xRRGGBB, gauge arc track; used when bg_set */
    char label[APP_SLOT_LABEL_MAX];
    char text[APP_SLOT_TEXT_MAX];
} app_slot_t;

typedef enum {
    APP_CONN_DISCONNECTED,
    APP_CONN_CONNECTING,
    APP_CONN_SECURE,
} app_conn_status_t;

typedef void (*state_on_change_cb_t)(void);

/**
 * @brief Register the callback fired after any state_set_*() call actually
 *        changes something.
 *
 * render is the only intended subscriber: it takes the LVGL lock and
 * re-renders whatever changed. Every other module (ble, power_button, input
 * handling) only ever calls state_set_*() — none of them touch LVGL.
 */
void state_set_on_change_cb(state_on_change_cb_t cb);

app_screen_t state_get_active_screen(void);
void state_set_active_screen(app_screen_t screen);

/**
 * @brief Whether a battery is plugged in at all (as opposed to a genuinely
 *        depleted one) — see components/battery/battery.c for the (currently
 *        unverified-on-hardware) voltage threshold this is derived from.
 */
bool state_get_battery_present(void);
void state_set_battery_present(bool present);

/**
 * @brief Charge level 0-100. Only meaningful when state_get_battery_present().
 */
uint8_t state_get_battery_percent(void);
void state_set_battery_percent(uint8_t percent);

/**
 * @brief Battery sense voltage in millivolts, as read off the divider (see
 *        components/battery/battery.c). Meaningful regardless of
 *        state_get_battery_present() — it's the raw reading that presence
 *        itself is derived from.
 */
uint16_t state_get_battery_voltage_mv(void);
void state_set_battery_voltage_mv(uint16_t mv);

app_conn_status_t state_get_connection_status(void);
void state_set_connection_status(app_conn_status_t status);

/**
 * @brief True when no GATT control write has arrived for 75s.
 *
 * Set from the BLE silence watchdog, cleared on the next control write.
 * Independent of connection/pairing: a live encrypted link with a silent
 * client is still stale. Render uses this for the dim red LED overlay.
 */
bool state_get_ble_data_stale(void);
void state_set_ble_data_stale(bool stale);

/**
 * @brief Whether NimBLE currently has at least one bonded peer in NVS.
 *
 * Set from ble on host sync (restored bonds), after a successful new bond,
 * and after Forget. Default is false until ble_app_on_sync runs.
 */
bool state_get_ble_paired(void);
void state_set_ble_paired(bool paired);

/**
 * @brief Passkey currently shown for a user-initiated pairing session.
 *
 * Visible is independent of the numeric value: a generated PIN may be
 * 000000, which is still a real passkey to display.
 */
bool state_get_ble_passkey_visible(void);
uint32_t state_get_ble_passkey(void);
void state_set_ble_passkey(uint32_t passkey);
void state_clear_ble_passkey(void);

/**
 * @brief Copy one main-screen slot. `index` is 0..APP_SLOT_COUNT-1;
 *        out-of-range returns an empty slot.
 */
void state_get_slot(uint8_t index, app_slot_t *out);
void state_get_slots(app_slot_t out[APP_SLOT_COUNT]);

/**
 * @brief Replace slots. `state_set_slot` updates one index;
 *        `state_set_slots` replaces all four and notifies once.
 *
 * BLE control writes go through these (not events_post): the payload is
 * data, not a user action, and events cannot depend on ble (ble already
 * depends on events for Pair/Forget).
 */
void state_set_slot(uint8_t index, const app_slot_t *slot);
void state_set_slots(const app_slot_t slots[APP_SLOT_COUNT]);

#ifdef __cplusplus
}
#endif
