#include "gatt_svc.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ble.h"
#include "rtc.h"
#include "state.h"

#define MAX_JSON_LEN 512

static const char *TAG = "gatt_svc";

static const ble_uuid128_t s_svc_uuid = GATT_SVC_UUID;
static const ble_uuid128_t s_control_chr_uuid = GATT_CONTROL_CHR_UUID;

static const char *s_slot_keys[APP_SLOT_COUNT] = {
    "slot_1", "slot_2", "slot_3", "slot_4",
};

static uint8_t clamp_percent(double v)
{
    if (v < 0.0) {
        return 0;
    }
    if (v > 100.0) {
        return 100;
    }
    return (uint8_t)(v + 0.5);
}

static void copy_json_string(char *dst, size_t dst_sz, const cJSON *item)
{
    if (!cJSON_IsString(item) || !item->valuestring) {
        return;
    }
    strlcpy(dst, item->valuestring, dst_sz);
}

// "#RRGGBB" or "RRGGBB". Rejects anything that isn't exactly 6 hex digits.
static bool parse_hex_rgb(const char *s, uint32_t *out)
{
    if (!s) {
        return false;
    }
    if (s[0] == '#') {
        s++;
    }
    if (s[0] == '\0') {
        return false;
    }
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 16);
    if (end != s + 6 || *end != '\0' || v > 0xFFFFFFul) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static void apply_slot_color(uint32_t *dst, bool *set, const cJSON *item, const char *key)
{
    if (!item) {
        return;
    }
    if (!cJSON_IsString(item) || !item->valuestring) {
        ESP_LOGW(TAG, "ignoring non-string %s", key);
        return;
    }
    uint32_t rgb = 0;
    if (!parse_hex_rgb(item->valuestring, &rgb)) {
        ESP_LOGW(TAG, "ignoring invalid %s '%s'", key, item->valuestring);
        return;
    }
    *dst = rgb;
    *set = true;
}

static void apply_slot_json(app_slot_t *slot, const cJSON *obj)
{
    const cJSON *type_item = cJSON_GetObjectItemCaseSensitive(obj, "type");
    if (cJSON_IsString(type_item) && type_item->valuestring) {
        const char *type = type_item->valuestring;
        if (strcmp(type, "gauge") == 0) {
            slot->type = APP_SLOT_GAUGE;
        } else if (strcmp(type, "text") == 0) {
            slot->type = APP_SLOT_TEXT;
        } else if (strcmp(type, "empty") == 0) {
            memset(slot, 0, sizeof(*slot));
            return;
        } else {
            ESP_LOGW(TAG, "unknown slot type '%s'", type);
            return;
        }
    }

    const cJSON *value = cJSON_GetObjectItemCaseSensitive(obj, "value");
    if (cJSON_IsNumber(value)) {
        slot->value = clamp_percent(value->valuedouble);
    }

    copy_json_string(slot->label, sizeof(slot->label),
                     cJSON_GetObjectItemCaseSensitive(obj, "label"));
    copy_json_string(slot->text, sizeof(slot->text),
                     cJSON_GetObjectItemCaseSensitive(obj, "text"));
    apply_slot_color(&slot->fg, &slot->fg_set,
                     cJSON_GetObjectItemCaseSensitive(obj, "fg"), "fg");
    apply_slot_color(&slot->bg, &slot->bg_set,
                     cJSON_GetObjectItemCaseSensitive(obj, "bg"), "bg");
}

// JSON is a merge patch: omitted slots stay as they are. A slot key set to
// null or {"type":"empty"} clears that cell. unix/tz_offset (computer clock)
// is applied via rtc_clock_set_unix(), not state. Dispatch of slots is
// state_set_slots() — not LVGL, and not events_post() (ble cannot depend
// on events).
static void handle_json_command(const char *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        ESP_LOGW(TAG, "ignoring malformed JSON");
        return;
    }

    ESP_LOGI(TAG, "control write: %s", json_str);

    const cJSON *unix_item = cJSON_GetObjectItemCaseSensitive(root, "unix");
    if (cJSON_IsNumber(unix_item)) {
        int32_t tz_offset = 0;
        const cJSON *tz_item = cJSON_GetObjectItemCaseSensitive(root, "tz_offset");
        if (cJSON_IsNumber(tz_item)) {
            tz_offset = (int32_t)tz_item->valuedouble;
        }
        esp_err_t err = rtc_clock_set_unix((int64_t)unix_item->valuedouble, tz_offset);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "rtc_clock_set_unix failed: %s", esp_err_to_name(err));
        }
    }

    app_slot_t slots[APP_SLOT_COUNT];
    state_get_slots(slots);

    bool any = false;
    for (uint8_t i = 0; i < APP_SLOT_COUNT; i++) {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(root, s_slot_keys[i]);
        if (!item) {
            continue;
        }
        any = true;
        if (cJSON_IsNull(item)) {
            memset(&slots[i], 0, sizeof(slots[i]));
            continue;
        }
        if (!cJSON_IsObject(item)) {
            ESP_LOGW(TAG, "ignoring non-object %s", s_slot_keys[i]);
            continue;
        }
        apply_slot_json(&slots[i], item);
    }

    if (any) {
        state_set_slots(slots);
    }

    cJSON_Delete(root);
}

static int control_chr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
    if (om_len == 0 || om_len >= MAX_JSON_LEN) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    char buf[MAX_JSON_LEN];
    uint16_t out_len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, &out_len) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    buf[out_len] = '\0';

    handle_json_command(buf);
    ble_app_on_control_write();
    return 0;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_control_chr_uuid.u,
                .access_cb = control_chr_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                0, /* No more characteristics in this service. */
            },
        },
    },
    {
        0, /* No more services. */
    },
};

void gatt_svc_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    (void)ctxt;
    (void)arg;
}

int gatt_svc_init(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        return rc;
    }

    return ble_gatts_add_svcs(s_gatt_svcs);
}
