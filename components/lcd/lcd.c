#include "lcd.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_io_expander_tca9554.h"
#include "esp_lcd_axs15231b.h"
#include "esp_lcd_io_spi.h"
#include "esp_log.h"

static const char *TAG = "lcd";

// Waveshare ESP32-S3-Touch-LCD-3.49 (V2 revision) wiring.
// GPIO21, which the V1-era examples call "LCD_RST", is actually LCD_TE
// (tearing effect) on V2 — unused here. Real LCD reset lives on the
// TCA9554 I/O expander instead (see below); GPIO8 empirically drives the
// backlight correctly on this board despite the V2 schematic calling it
// "EXIO_INT", so it's left as-is.
#define PIN_NUM_LCD_PCLK    GPIO_NUM_10
#define PIN_NUM_LCD_CS      GPIO_NUM_9
#define PIN_NUM_LCD_DATA0   GPIO_NUM_11
#define PIN_NUM_LCD_DATA1   GPIO_NUM_12
#define PIN_NUM_LCD_DATA2   GPIO_NUM_13
#define PIN_NUM_LCD_DATA3   GPIO_NUM_14
#define PIN_NUM_LCD_BL      GPIO_NUM_8

#define PIN_NUM_TOUCH_SCL   GPIO_NUM_18
#define PIN_NUM_TOUCH_SDA   GPIO_NUM_17
#define TOUCH_I2C_PORT      I2C_NUM_1
#define TOUCH_I2C_ADDR      0x3B

// System I2C bus: TCA9554 expander (LCD reset/backlight-enable), PCF85063
// RTC (components/rtc), plus IMU/audio codecs.
#define PIN_NUM_SYS_SCL     GPIO_NUM_48
#define PIN_NUM_SYS_SDA     GPIO_NUM_47
#define SYS_I2C_PORT        I2C_NUM_0
#define TCA9554_I2C_ADDR    ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000

#define EXIO_BIT_TOUCH_INT  (1ULL << 0)
#define EXIO_BIT_BL_EN      (1ULL << 1)
#define EXIO_BIT_LCD_RST    (1ULL << 5)
// Power-hold latch (verified against Waveshare's own factory firmware
// tca9554_init()): the PWR button only *starts* the boot by momentarily
// bridging power — firmware must drive this pin high to keep the board
// powered once the button is released, and low to cut power entirely.
#define EXIO_BIT_PWR_HOLD   (1ULL << 6)

#define LCD_SPI_HOST        SPI3_HOST

#define LEDC_BL_TIMER      LEDC_TIMER_0
#define LEDC_BL_MODE       LEDC_LOW_SPEED_MODE
#define LEDC_BL_CHANNEL    LEDC_CHANNEL_0
#define LEDC_BL_DUTY_RES   LEDC_TIMER_10_BIT

// AXS15231B touch read command, verified against this exact board's factory
// firmware. Response: buff[1] is the touch-point count (0 = released),
// buff[2..3] and buff[4..5] are the raw 12-bit X/Y (big-endian, top nibble
// masked).
static const uint8_t s_touch_read_cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00};

static esp_io_expander_handle_t s_expander;
static i2c_master_dev_handle_t s_touch_dev;
static uint8_t s_backlight_percent = 50;
static bool s_backlight_on = true;

// Verified against Waveshare's own V2 factory firmware main.cpp: sleep-out +
// display-on is genuinely all this panel needs (the driver's larger bundled
// gamma/power/timing sequence was a red herring chased from an unrelated
// board's bug report).
static const axs15231b_lcd_init_cmd_t s_lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 100},
    {0x29, (uint8_t[]){0x00}, 0, 100},
};

static void backlight_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_BL_MODE,
        .timer_num = LEDC_BL_TIMER,
        .duty_resolution = LEDC_BL_DUTY_RES,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t channel_cfg = {
        .gpio_num = PIN_NUM_LCD_BL,
        .speed_mode = LEDC_BL_MODE,
        .channel = LEDC_BL_CHANNEL,
        .timer_sel = LEDC_BL_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel_cfg));
}

void lcd_backlight_set(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    uint32_t max_duty = (1 << LEDC_BL_DUTY_RES) - 1;
    uint32_t duty = (max_duty * percent) / 100;
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_BL_MODE, LEDC_BL_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_BL_MODE, LEDC_BL_CHANNEL));

    if (s_expander) {
        ESP_ERROR_CHECK(esp_io_expander_set_level(s_expander, EXIO_BIT_BL_EN, percent > 0 ? 1 : 0));
    }

    s_backlight_on = percent > 0;
    if (s_backlight_on) {
        s_backlight_percent = percent;
    }
}

void lcd_backlight_toggle(void)
{
    lcd_backlight_set(s_backlight_on ? 0 : s_backlight_percent);
}

void lcd_power_off(void)
{
    lcd_backlight_set(0);
    if (s_expander) {
        ESP_ERROR_CHECK(esp_io_expander_set_level(s_expander, EXIO_BIT_PWR_HOLD, 0));
    }
}

// Brings up the system I2C bus and the TCA9554 expander that actually holds
// LCD_RST (bit 5) on this board revision, then performs the real reset pulse
// through it. A bare-GPIO pulse on GPIO21 (what older examples call
// "LCD_RST") never reset anything here — that pin is LCD_TE on this
// revision — which is why the panel only ever came up cleanly after a true
// power cycle (the expander's own power-on reset) and not after a soft/EN
// reset (the expander keeps its register state across those).
//
// BL_EN is left off here and only turned on afterwards, via lcd_backlight_set
// — matching the factory firmware's ordering, which never touches the
// backlight-enable gate until after the panel is reset and initialized.
static i2c_master_bus_handle_t s_sys_i2c_bus;

static esp_io_expander_handle_t expander_init_and_reset(void)
{
    ESP_LOGI(TAG, "Initializing system I2C bus / TCA9554 expander");
    i2c_master_bus_config_t sys_i2c_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = SYS_I2C_PORT,
        .scl_io_num = PIN_NUM_SYS_SCL,
        .sda_io_num = PIN_NUM_SYS_SDA,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&sys_i2c_cfg, &s_sys_i2c_bus));

    esp_io_expander_handle_t expander = NULL;
    ESP_ERROR_CHECK(esp_io_expander_new_i2c_tca9554(s_sys_i2c_bus, TCA9554_I2C_ADDR, &expander));

    // Latch power on before anything else on this bus: the board is still
    // only powered by the user's finger on the PWR button at this point, so
    // every microsecond spent on other setup below is time the button has to
    // stay held before release would cut power out from under us.
    ESP_ERROR_CHECK(esp_io_expander_set_dir(expander, EXIO_BIT_PWR_HOLD, IO_EXPANDER_OUTPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_level(expander, EXIO_BIT_PWR_HOLD, 1));

    ESP_ERROR_CHECK(esp_io_expander_set_dir(expander, EXIO_BIT_TOUCH_INT, IO_EXPANDER_INPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_dir(expander, EXIO_BIT_BL_EN | EXIO_BIT_LCD_RST, IO_EXPANDER_OUTPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_level(expander, EXIO_BIT_BL_EN, 0));
    ESP_ERROR_CHECK(esp_io_expander_set_level(expander, EXIO_BIT_LCD_RST, 1));

    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_ERROR_CHECK(esp_io_expander_set_level(expander, EXIO_BIT_LCD_RST, 0));
    vTaskDelay(pdMS_TO_TICKS(250));
    ESP_ERROR_CHECK(esp_io_expander_set_level(expander, EXIO_BIT_LCD_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(30));

    return expander;
}

// The AXS15231B's touch block doesn't speak the generic esp_lcd_touch/
// esp_lcd_panel_io_i2c framing (esp_lcd_touch_new_i2c_axs15231b() NAKs on
// every transaction here). Waveshare's own working firmware talks to it with
// a plain I2C device handle and a fixed magic command instead — same
// approach, verified against this exact board.
static void touch_init(void)
{
    ESP_LOGI(TAG, "Initializing touch I2C bus");
    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = TOUCH_I2C_PORT,
        .scl_io_num = PIN_NUM_TOUCH_SCL,
        .sda_io_num = PIN_NUM_TOUCH_SDA,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &i2c_bus));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_I2C_ADDR,
        .scl_speed_hz = 300000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &dev_cfg, &s_touch_dev));
}

esp_err_t lcd_touch_read(uint16_t *x, uint16_t *y, bool *pressed)
{
    uint8_t buff[32] = {0};
    esp_err_t err = i2c_master_transmit_receive(s_touch_dev, s_touch_read_cmd, sizeof(s_touch_read_cmd),
                                                 buff, sizeof(buff), 100);
    if (err != ESP_OK) {
        *pressed = false;
        return err;
    }

    if (buff[1] == 0 || buff[1] >= 5) {
        *pressed = false;
        return ESP_OK;
    }

    uint16_t raw_x = (((uint16_t)buff[2] & 0x0f) << 8) | (uint16_t)buff[3];
    uint16_t raw_y = (((uint16_t)buff[4] & 0x0f) << 8) | (uint16_t)buff[5];
    if (raw_x >= LCD_V_RES) {
        raw_x = LCD_V_RES - 1;
    }
    if (raw_y >= LCD_H_RES) {
        raw_y = LCD_H_RES - 1;
    }

    *x = raw_x;
    *y = raw_y;
    *pressed = true;
    return ESP_OK;
}

void lcd_init(lcd_handle_t *out_lcd)
{
    backlight_init();

    // A soft/EN reset doesn't power-cycle the board's shared 3.3V rail the
    // way unplugging USB does; give it a moment to settle before touching
    // anything.
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Initializing QSPI bus");
    spi_bus_config_t buscfg = AXS15231B_PANEL_BUS_QSPI_CONFIG(
        PIN_NUM_LCD_PCLK, PIN_NUM_LCD_DATA0, PIN_NUM_LCD_DATA1,
        PIN_NUM_LCD_DATA2, PIN_NUM_LCD_DATA3,
        LCD_H_RES * 80 * sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Installing panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = AXS15231B_PANEL_IO_QSPI_CONFIG(PIN_NUM_LCD_CS, NULL, NULL);
    // Color buffers are in PSRAM. Direct PSRAM DMA avoids a ~chunk-sized
    // internal bounce alloc that fails once the BT controller is up.
    // 40 MHz (the vendor default) underflows on this octal-PSRAM bus
    // (white/yellow stripes, LVGL stuck in wait_for_flushing, dead touch).
    io_config.flags.psram_dma_direct = 1;
    io_config.pclk_hz = 10 * 1000 * 1000;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &io_handle));

    ESP_LOGI(TAG, "Installing AXS15231B panel driver");
    esp_lcd_panel_handle_t panel_handle = NULL;
    axs15231b_vendor_config_t vendor_config = {
        .init_cmds = s_lcd_init_cmds,
        .init_cmds_size = sizeof(s_lcd_init_cmds) / sizeof(s_lcd_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    // reset_gpio_num = -1: reset is done through the TCA9554 expander below,
    // not a bare ESP32 GPIO.
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(io_handle, &panel_config, &panel_handle));

    s_expander = expander_init_and_reset();

    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    touch_init();

    out_lcd->io_handle = io_handle;
    out_lcd->panel_handle = panel_handle;
    out_lcd->sys_i2c_bus = s_sys_i2c_bus;

    lcd_backlight_set(50);
}
