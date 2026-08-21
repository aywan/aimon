#pragma once

#include "host/ble_gatt.h"
#include "host/ble_uuid.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 128-bit UUIDs, randomly generated for this project.
 * Human-readable form (for the Go client / phone apps):
 *   service:            b128a90e-9917-4c69-b86f-d1a5df5a88d0
 *   control (write):    3aa26982-1c4a-4563-b78c-0ed9d9e07526
 */
#define GATT_SVC_UUID \
    BLE_UUID128_INIT(0xd0, 0x88, 0x5a, 0xdf, 0xa5, 0xd1, 0x6f, 0xb8, \
                      0x69, 0x4c, 0x17, 0x99, 0x0e, 0xa9, 0x28, 0xb1)

#define GATT_CONTROL_CHR_UUID \
    BLE_UUID128_INIT(0x26, 0x75, 0xe0, 0xd9, 0xd9, 0x0e, 0x8c, 0xb7, \
                      0x63, 0x45, 0x4a, 0x1c, 0x82, 0x69, 0xa2, 0x3a)

int gatt_svc_init(void);
void gatt_svc_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg);

#ifdef __cplusplus
}
#endif
