#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_EVENT_SCREEN_NEXT,
    APP_EVENT_SCREEN_PREV,
    APP_EVENT_BLE_PAIR,
    APP_EVENT_BLE_FORGET,
} app_event_t;

/**
 * @brief Single entry point for user-triggered actions.
 *
 * Every input source (touch gestures, Settings buttons, future BLE commands)
 * posts here instead of mutating state or calling other modules directly.
 * Keeps "what triggered this" separate from "what it does" (state_set_*()
 * and any other module call a given event should trigger — Pair/Forget
 * reach into the ble component here).
 */
void events_post(app_event_t event);

#ifdef __cplusplus
}
#endif
