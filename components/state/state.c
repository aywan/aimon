#include "state.h"

#include <string.h>

static app_screen_t s_active_screen = APP_SCREEN_MAIN;
static app_slot_t s_slots[APP_SLOT_COUNT];
static bool s_battery_present;
static uint8_t s_battery_percent;
static uint16_t s_battery_voltage_mv;
static app_conn_status_t s_connection_status = APP_CONN_DISCONNECTED;
static bool s_ble_data_stale;
static bool s_ble_paired;
static bool s_ble_passkey_visible;
static uint32_t s_ble_passkey;
static state_on_change_cb_t s_on_change;

static void notify(void)
{
    if (s_on_change) {
        s_on_change();
    }
}

void state_set_on_change_cb(state_on_change_cb_t cb)
{
    s_on_change = cb;
}

app_screen_t state_get_active_screen(void)
{
    return s_active_screen;
}

void state_set_active_screen(app_screen_t screen)
{
    if (screen == s_active_screen) {
        return;
    }
    s_active_screen = screen;
    notify();
}

bool state_get_battery_present(void)
{
    return s_battery_present;
}

void state_set_battery_present(bool present)
{
    if (present == s_battery_present) {
        return;
    }
    s_battery_present = present;
    notify();
}

uint8_t state_get_battery_percent(void)
{
    return s_battery_percent;
}

void state_set_battery_percent(uint8_t percent)
{
    if (percent == s_battery_percent) {
        return;
    }
    s_battery_percent = percent;
    notify();
}

uint16_t state_get_battery_voltage_mv(void)
{
    return s_battery_voltage_mv;
}

void state_set_battery_voltage_mv(uint16_t mv)
{
    if (mv == s_battery_voltage_mv) {
        return;
    }
    s_battery_voltage_mv = mv;
    notify();
}

app_conn_status_t state_get_connection_status(void)
{
    return s_connection_status;
}

void state_set_connection_status(app_conn_status_t status)
{
    if (status == s_connection_status) {
        return;
    }
    s_connection_status = status;
    notify();
}

bool state_get_ble_data_stale(void)
{
    return s_ble_data_stale;
}

void state_set_ble_data_stale(bool stale)
{
    if (stale == s_ble_data_stale) {
        return;
    }
    s_ble_data_stale = stale;
    notify();
}

bool state_get_ble_paired(void)
{
    return s_ble_paired;
}

void state_set_ble_paired(bool paired)
{
    if (paired == s_ble_paired) {
        return;
    }
    s_ble_paired = paired;
    notify();
}

bool state_get_ble_passkey_visible(void)
{
    return s_ble_passkey_visible;
}

uint32_t state_get_ble_passkey(void)
{
    return s_ble_passkey;
}

void state_set_ble_passkey(uint32_t passkey)
{
    if (s_ble_passkey_visible && s_ble_passkey == passkey) {
        return;
    }
    s_ble_passkey_visible = true;
    s_ble_passkey = passkey;
    notify();
}

void state_clear_ble_passkey(void)
{
    if (!s_ble_passkey_visible) {
        return;
    }
    s_ble_passkey_visible = false;
    s_ble_passkey = 0;
    notify();
}

void state_get_slot(uint8_t index, app_slot_t *out)
{
    if (!out) {
        return;
    }
    if (index >= APP_SLOT_COUNT) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = s_slots[index];
}

void state_get_slots(app_slot_t out[APP_SLOT_COUNT])
{
    if (!out) {
        return;
    }
    memcpy(out, s_slots, sizeof(s_slots));
}

void state_set_slot(uint8_t index, const app_slot_t *slot)
{
    if (!slot || index >= APP_SLOT_COUNT) {
        return;
    }
    if (memcmp(&s_slots[index], slot, sizeof(*slot)) == 0) {
        return;
    }
    s_slots[index] = *slot;
    notify();
}

void state_set_slots(const app_slot_t slots[APP_SLOT_COUNT])
{
    if (!slots) {
        return;
    }
    if (memcmp(s_slots, slots, sizeof(s_slots)) == 0) {
        return;
    }
    memcpy(s_slots, slots, sizeof(s_slots));
    notify();
}
