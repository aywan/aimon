#include "events.h"

#include "ble.h"
#include "state.h"

void events_post(app_event_t event)
{
    app_screen_t active = state_get_active_screen();

    switch (event) {
    case APP_EVENT_SCREEN_NEXT:
        state_set_active_screen((active + 1) % APP_SCREEN_COUNT);
        break;
    case APP_EVENT_SCREEN_PREV:
        state_set_active_screen((active - 1 + APP_SCREEN_COUNT) % APP_SCREEN_COUNT);
        break;
    case APP_EVENT_BLE_PAIR:
        ble_app_begin_pairing();
        break;
    case APP_EVENT_BLE_FORGET:
        ble_app_forget_paired();
        break;
    }
}
