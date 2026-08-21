#include "ble.h"

#include <assert.h>
#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "gatt_svc.h"
#include "state.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

#define NOTIFY_STALE_MS    (75 * 1000)
#define WATCHDOG_TICK_US   (1000 * 1000)
#define CONN_HANDLE_NONE   0xFFFF

static const char *TAG = "ble_app";

static uint8_t s_own_addr_type;
static int gap_event_cb(struct ble_gap_event *event, void *arg);
void ble_store_config_init(void);

static volatile bool s_status_stale;
static volatile uint32_t s_last_activity_ms;
static uint16_t s_conn_handle = CONN_HANDLE_NONE;
static volatile bool s_pairing_open;
static uint32_t s_pairing_passkey;
static esp_timer_handle_t s_watchdog_timer;
static esp_timer_handle_t s_pairing_timer;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static int bonded_peer_count(void);

static void mark_activity(void)
{
    s_last_activity_ms = now_ms();
    s_status_stale = false;
    state_set_ble_data_stale(false);
}

static void mark_link_down(void)
{
    s_conn_handle = CONN_HANDLE_NONE;
    state_set_connection_status(APP_CONN_DISCONNECTED);
}

static void watchdog_cb(void *arg)
{
    (void)arg;

    if (s_status_stale) {
        return;
    }

    uint32_t elapsed = now_ms() - s_last_activity_ms;
    if (elapsed < NOTIFY_STALE_MS) {
        return;
    }

    s_status_stale = true;
    state_set_ble_data_stale(true);
    ESP_LOGW(TAG, "no control writes for %lu ms; data stale", (unsigned long)elapsed);
}

void ble_app_on_control_write(void)
{
    mark_activity();
}

bool ble_app_pairing_window_open(void)
{
    return s_pairing_open;
}

static void refresh_paired_state(void)
{
    state_set_ble_paired(bonded_peer_count() > 0);
}

static void close_pairing_window(void)
{
    s_pairing_open = false;
    s_pairing_passkey = 0;
    if (s_pairing_timer) {
        esp_timer_stop(s_pairing_timer);
    }
    state_clear_ble_passkey();
}

static void pairing_timeout_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "pairing window closed");
    close_pairing_window();
}

void ble_app_begin_pairing(void)
{
    if (bonded_peer_count() > 0) {
        ESP_LOGI(TAG, "pair ignored: already bonded");
        return;
    }

    s_pairing_passkey = esp_random() % 1000000;
    s_pairing_open = true;
    if (s_pairing_timer) {
        esp_timer_stop(s_pairing_timer);
        ESP_ERROR_CHECK(esp_timer_start_once(
            s_pairing_timer, (uint64_t)BLE_APP_PAIRING_WINDOW_SEC * 1000000));
    }
    ESP_LOGI(TAG, "pairing window open %ds; passkey %06" PRIu32,
             BLE_APP_PAIRING_WINDOW_SEC, s_pairing_passkey);
    state_set_ble_passkey(s_pairing_passkey);
}

void ble_app_forget_paired(void)
{
    ble_addr_t peers[8];
    int num_peers = 0;

    close_pairing_window();

    if (s_conn_handle != CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }

    if (ble_store_util_bonded_peers(peers, &num_peers, 8) == 0) {
        for (int i = 0; i < num_peers; i++) {
            ble_store_util_delete_peer(&peers[i]);
        }
    }

    ESP_LOGI(TAG, "forgot bonded peer(s)");
    state_set_ble_paired(false);
}

static int bonded_peer_count(void)
{
    ble_addr_t peers[8];
    int num_peers = 0;

    if (ble_store_util_bonded_peers(peers, &num_peers, 8) != 0) {
        return 0;
    }
    return num_peers;
}

static bool peer_is_bonded(const ble_addr_t *addr)
{
    ble_addr_t peers[8];
    int num_peers = 0;

    if (addr == NULL || ble_store_util_bonded_peers(peers, &num_peers, 8) != 0) {
        return false;
    }
    for (int i = 0; i < num_peers; i++) {
        if (ble_addr_cmp(&peers[i], addr) == 0) {
            return true;
        }
    }
    return false;
}

static bool conn_looks_bonded(const struct ble_gap_conn_desc *desc)
{
    return peer_is_bonded(&desc->peer_id_addr) || peer_is_bonded(&desc->peer_ota_addr);
}

/* macOS reconnects with an RPA; identity may not be resolved at CONNECT. */
static bool conn_maybe_bonded_rpa(const struct ble_gap_conn_desc *desc)
{
    return bonded_peer_count() > 0 &&
           (BLE_ADDR_IS_RPA(&desc->peer_id_addr) || BLE_ADDR_IS_RPA(&desc->peer_ota_addr));
}

static void ble_app_advertise(void)
{
    struct ble_hs_adv_fields fields;
    struct ble_hs_adv_fields rsp_fields;
    struct ble_gap_adv_params adv_params;
    const char *name;
    int rc;

    /* Legacy advertising payloads are capped at 31 bytes, so the 128-bit
     * service UUID and the device name don't both fit in one packet. The
     * complete local name goes in the primary adv data: CoreBluetooth and
     * most "device lists" never show a name that lives only in the scan
     * response. UUID goes in the scan response. */
    memset(&fields, 0, sizeof(fields));
    name = ble_svc_gap_device_name();
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "error setting advertisement data; rc=%d", rc);
        return;
    }

    memset(&rsp_fields, 0, sizeof(rsp_fields));
    rsp_fields.uuids128 = (ble_uuid128_t[]) { GATT_SVC_UUID };
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 1;
    rsp_fields.tx_pwr_lvl_is_present = 1;
    rsp_fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "error setting scan response data; rc=%d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "error enabling advertisement; rc=%d", rc);
    }
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    int rc;

    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connection failed; status=%d", event->connect.status);
            ble_app_advertise();
            return 0;
        }

        rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
        assert(rc == 0);

        if (!conn_looks_bonded(&desc) && !conn_maybe_bonded_rpa(&desc) &&
            !ble_app_pairing_window_open()) {
            ESP_LOGW(TAG, "rejecting new peer: pairing window closed");
            ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }

        ESP_LOGI(TAG, "connected; conn_handle=%d bonded=%d",
                 event->connect.conn_handle, (int)conn_looks_bonded(&desc));
        s_conn_handle = event->connect.conn_handle;
        state_set_connection_status(APP_CONN_CONNECTING);

        /* Only if identity is already known — otherwise this starts a new
         * pairing instead of LTK encryption, which macOS will refuse. */
        if (conn_looks_bonded(&desc)) {
            rc = ble_gap_security_initiate(event->connect.conn_handle);
            if (rc != 0) {
                ESP_LOGW(TAG, "ble_gap_security_initiate rc=%d", rc);
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected; reason=%d", event->disconnect.reason);
        mark_link_down();
        ble_app_advertise();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
        assert(rc == 0);
        ESP_LOGI(TAG, "encryption change; status=%d encrypted=%d bonded=%d",
                 event->enc_change.status, desc.sec_state.encrypted, desc.sec_state.bonded);
        if (event->enc_change.status == 0 && desc.sec_state.encrypted) {
            state_set_connection_status(APP_CONN_SECURE);
            if (desc.sec_state.bonded) {
                state_set_ble_paired(true);
                close_pairing_window();
            }
            return 0;
        }
        if (event->enc_change.status != 0 && !ble_app_pairing_window_open()) {
            /* macOS encrypts with LTK immediately, often while the peer is
             * still an unresolved RPA. Dropping here races identity
             * resolution and looks like "pairing did not survive reboot". */
            if (conn_looks_bonded(&desc) || conn_maybe_bonded_rpa(&desc)) {
                ESP_LOGW(TAG, "encryption failed before identity resolved; keeping link");
                return 0;
            }
            ESP_LOGW(TAG, "encryption failed and pairing window closed; dropping");
            ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_AUTH_FAIL);
        }
        return 0;

#ifdef BLE_GAP_EVENT_IDENTITY_RESOLVED
    case BLE_GAP_EVENT_IDENTITY_RESOLVED:
        rc = ble_gap_conn_find(event->identity_resolved.conn_handle, &desc);
        if (rc != 0) {
            return 0;
        }
        ESP_LOGI(TAG, "identity resolved; bonded=%d encrypted=%d",
                 (int)conn_looks_bonded(&desc), desc.sec_state.encrypted);
        if (conn_looks_bonded(&desc) && !desc.sec_state.encrypted) {
            rc = ble_gap_security_initiate(event->identity_resolved.conn_handle);
            if (rc != 0) {
                ESP_LOGW(TAG, "ble_gap_security_initiate rc=%d", rc);
            }
        } else if (!conn_looks_bonded(&desc) && !ble_app_pairing_window_open()) {
            ESP_LOGW(TAG, "resolved identity is not bonded; dropping");
            ble_gap_terminate(event->identity_resolved.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;
#endif

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ble_app_advertise();
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* bleprph always deletes and retries. That is wrong once pairing is
         * gated by the Pair button: after reboot the window is closed, so
         * RETRY would wipe the NVS bond and then PASSKEY_ACTION would reject
         * the new pairing. Keep the stored LTK unless the user asked to pair. */
        if (!ble_app_pairing_window_open()) {
            ESP_LOGW(TAG, "repeat pairing ignored: window closed (keeping bond)");
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        assert(rc == 0);
        ble_store_util_delete_peer(&desc.peer_id_addr);
        refresh_paired_state();
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (!ble_app_pairing_window_open()) {
            ESP_LOGW(TAG, "rejecting pairing: window closed");
            ble_gap_terminate(event->passkey.conn_handle, BLE_ERR_AUTH_FAIL);
            return 0;
        }
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            struct ble_sm_io pkey = { .action = BLE_SM_IOACT_DISP, .passkey = s_pairing_passkey };

            ESP_LOGI(TAG, "injecting passkey %06" PRIu32, s_pairing_passkey);
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            if (rc != 0) {
                ESP_LOGE(TAG, "ble_sm_inject_io failed; rc=%d", rc);
            }
        }
        return 0;

    default:
        return 0;
    }
}

static void ble_app_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "error determining address type; rc=%d", rc);
        return;
    }

    uint8_t addr[6];
    rc = ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG,
                 "host synced; addr_type=%u %02x:%02x:%02x:%02x:%02x:%02x; restored %d bonded peer(s)",
                 s_own_addr_type, addr[5], addr[4], addr[3], addr[2], addr[1], addr[0],
                 bonded_peer_count());
    } else {
        ESP_LOGI(TAG, "host synced; restored %d bonded peer(s)", bonded_peer_count());
    }
    refresh_paired_state();
    ble_app_advertise();
}

static void ble_app_on_reset(int reason)
{
    ESP_LOGW(TAG, "nimble host reset; reason=%d", reason);
}

static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_app_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs_flash_init rc=%s; erasing NVS (BLE bonds will be lost)",
                 esp_err_to_name(ret));
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.reset_cb = ble_app_on_reset;
    ble_hs_cfg.sync_cb = ble_app_on_sync;
    ble_hs_cfg.gatts_register_cb = gatt_svc_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* We only have a screen, no input: peer enters a passkey we display. */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ESP_ERROR_CHECK(gatt_svc_init());
    ESP_ERROR_CHECK(ble_svc_gap_device_name_set(BLE_APP_DEVICE_NAME));

    ble_store_config_init();

    nimble_port_freertos_init(ble_host_task);

    const esp_timer_create_args_t watchdog_args = {
        .callback = watchdog_cb,
        .name = "ble_watchdog",
    };
    ESP_ERROR_CHECK(esp_timer_create(&watchdog_args, &s_watchdog_timer));
    s_last_activity_ms = now_ms();
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_watchdog_timer, WATCHDOG_TICK_US));

    const esp_timer_create_args_t pairing_args = {
        .callback = pairing_timeout_cb,
        .name = "ble_pair_win",
    };
    ESP_ERROR_CHECK(esp_timer_create(&pairing_args, &s_pairing_timer));

    ESP_LOGI(TAG, "BLE ready as \"%s\"; pair from Settings (%ds window)",
             BLE_APP_DEVICE_NAME, BLE_APP_PAIRING_WINDOW_SEC);
}
