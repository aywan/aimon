#include "power_button.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lcd.h"

// Waveshare ESP32-S3-Touch-LCD-3.49: PWR button (separate from BOOT/GPIO0)
#define PWR_BTN_GPIO        GPIO_NUM_16
#define POLL_PERIOD_US      (20 * 1000)
#define LONG_PRESS_MS       1500

static const char *TAG = "power_button";

static esp_timer_handle_t s_poll_timer;
static int64_t s_press_start_us;
static bool s_pressed;
static bool s_long_press_fired;

// True only for the very first press episode seen after init — the latch
// needs the button held through boot, so if it's still down when we start
// polling, this is that same power-on press. Its release must not also be
// read as a short press, or it'd immediately toggle the screen (which
// lcd_init() already turned on) back off.
static bool s_boot_press;

static void poll_cb(void *arg)
{
    (void)arg;

    bool level_low = gpio_get_level(PWR_BTN_GPIO) == 0;

    if (level_low && !s_pressed) {
        s_pressed = true;
        s_long_press_fired = false;
        s_press_start_us = esp_timer_get_time();
    } else if (level_low && s_pressed && !s_long_press_fired) {
        int64_t held_ms = (esp_timer_get_time() - s_press_start_us) / 1000;
        if (held_ms >= LONG_PRESS_MS) {
            s_long_press_fired = true;
            ESP_LOGI(TAG, "long press detected; powering off");
            lcd_power_off(); // does not return
        }
    } else if (!level_low && s_pressed) {
        s_pressed = false;
        if (!s_long_press_fired && !s_boot_press) {
            ESP_LOGI(TAG, "short press detected; toggling screen");
            lcd_backlight_toggle();
        }
        s_boot_press = false;
    }
}

void power_button_init(void)
{
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << PWR_BTN_GPIO,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    if (gpio_get_level(PWR_BTN_GPIO) == 0) {
        s_pressed = true;
        s_boot_press = true;
        s_press_start_us = esp_timer_get_time();
    }

    const esp_timer_create_args_t timer_args = {
        .callback = poll_cb,
        .name = "pwr_btn_poll",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_poll_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_poll_timer, POLL_PERIOD_US));
}
