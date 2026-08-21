#include "esp_log.h"
#include "battery.h"
#include "ble.h"
#include "lcd.h"
#include "mic.h"
#include "power_button.h"
#include "render.h"
#include "rtc.h"

static const char *TAG = "main";

void app_main(void)
{
    lcd_handle_t lcd;
    lcd_init(&lcd);

    // BT controller allocates from internal DRAM only. LVGL's framebuffers
    // and I2S DMA eat that heap, so NimBLE has to come up before them —
    // otherwise nimble_port_init() returns ESP_ERR_NO_MEM and abort() loops.
    ble_app_init();

    rtc_clock_init(&lcd);
    render_init(&lcd);
    mic_init(&lcd);

    battery_init();
    power_button_init();

    ESP_LOGI(TAG, "Init done");
}
