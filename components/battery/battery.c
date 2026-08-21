#include "battery.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "state.h"

// Waveshare ESP32-S3-Touch-LCD-3.49 V2: battery sense on GPIO4 = ADC1_CH3,
// through an external divider — the factory ADC example computes
// battery_voltage = (calibrated_mV / 1000) * 3, confirmed straight from
// their own adc_bsp.c.
#define BAT_ADC_UNIT        ADC_UNIT_1
#define BAT_ADC_CHANNEL     ADC_CHANNEL_3
#define BAT_ADC_ATTEN       ADC_ATTEN_DB_12
#define BAT_ADC_BITWIDTH    ADC_BITWIDTH_12
#define BAT_DIVIDER_RATIO   3.0f

// Simple linear approximation of a single-cell LiPo's charge curve — good
// enough for a UI gauge, not a fuel-gauge IC's precision.
#define BAT_VOLTAGE_EMPTY   3.3f
#define BAT_VOLTAGE_FULL    4.2f

// Below this, nothing is plugged into the battery connector at all (as
// opposed to a genuinely depleted cell) — NOT verified against real
// hardware; adjust if a real battery is ever read this low, or if "no
// battery" turns out to float high instead of low on this board.
#define BAT_VOLTAGE_PRESENT_MIN  2.0f

#define POLL_PERIOD_US  (5 * 1000 * 1000)

static const char *TAG = "battery";
static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_cali_handle;
static esp_timer_handle_t s_poll_timer;

static float read_voltage(void)
{
    int raw = 0;
    if (adc_oneshot_read(s_adc_handle, BAT_ADC_CHANNEL, &raw) != ESP_OK) {
        return 0.0f;
    }
    int mv = 0;
    if (adc_cali_raw_to_voltage(s_cali_handle, raw, &mv) != ESP_OK) {
        return 0.0f;
    }
    return (mv / 1000.0f) * BAT_DIVIDER_RATIO;
}

static void poll_cb(void *arg)
{
    (void)arg;

    float voltage = read_voltage();
    state_set_battery_voltage_mv((uint16_t)(voltage * 1000.0f + 0.5f));

    if (voltage < BAT_VOLTAGE_PRESENT_MIN) {
        state_set_battery_present(false);
        return;
    }

    float pct = (voltage - BAT_VOLTAGE_EMPTY) / (BAT_VOLTAGE_FULL - BAT_VOLTAGE_EMPTY) * 100.0f;
    if (pct < 0.0f) {
        pct = 0.0f;
    } else if (pct > 100.0f) {
        pct = 100.0f;
    }

    ESP_LOGD(TAG, "%.2fV -> %d%%", voltage, (int)pct);
    state_set_battery_present(true);
    state_set_battery_percent((uint8_t)pct);
}

void battery_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = BAT_ADC_UNIT,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BAT_ADC_ATTEN,
        .bitwidth = BAT_ADC_BITWIDTH,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, BAT_ADC_CHANNEL, &chan_cfg));

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = BAT_ADC_UNIT,
        .atten = BAT_ADC_ATTEN,
        .bitwidth = BAT_ADC_BITWIDTH,
    };
    ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle));

    const esp_timer_create_args_t timer_args = {
        .callback = poll_cb,
        .name = "battery_poll",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_poll_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_poll_timer, POLL_PERIOD_US));

    poll_cb(NULL); // seed state immediately instead of waiting for the first tick
}
