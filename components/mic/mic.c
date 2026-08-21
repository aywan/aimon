#include "mic.h"

#include <math.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Waveshare ESP32-S3-Touch-LCD-3.49: ES7210 mic ADC, control I2C on the same
// system bus as the TCA9554 (lcd.c), audio data over a separate I2S bus.
// Pins/I2C address verified against Waveshare's own working example
// (waveshareteam/ESP32-S3-Touch-LCD-3.49, Examples/Arduino/08_Audio_Test,
// board_cfg.h entry "S3_LCD_3_49" — not in their generic board_cfg.txt list,
// only the board_cfg.h the .ino actually compiles in) — not derivable from
// the schematic alone. dout/GPIO45 (speaker path) is deliberately unused
// here, we only ever read.
#define ES7210_I2C_ADDR     0x40
#define MIC_I2S_PORT        I2S_NUM_0
#define MIC_MCLK_GPIO       GPIO_NUM_7
#define MIC_BCLK_GPIO       GPIO_NUM_15
#define MIC_WS_GPIO         GPIO_NUM_46
#define MIC_DIN_GPIO        GPIO_NUM_6

// I2S_STD_CLK_DEFAULT_CONFIG defaults to 256*Fs on MCLK. The ES7210 register
// values below (MAINCLK_REG02=0xC1, OSR_REG07=0x20) are the vendor driver's
// fixed operating point for that exact MCLK/Fs ratio (its clock-coefficient
// table's {12288000, 48000, ...} row decodes to those two register values) —
// changing MIC_SAMPLE_RATE_HZ without also re-deriving those two registers
// from that table will silently produce garbage or silence.
#define MIC_SAMPLE_RATE_HZ  48000

// Approximate-on-purpose (see mic.h): full-scale int16 is 32767, but real
// speech/noise RMS rarely gets near that with 30dB of mic gain at normal
// distances. This divisor is an unverified guess at "sounds loud on this
// board", not a calibrated SPL mapping — lower means more sensitive (a
// quieter sound reaches 100%).
#define NOISE_RMS_FULL_SCALE 500
#define NOISE_TASK_STACK     4096
#define NOISE_READ_FRAMES    256 // stereo frames per I2S read chunk (~5.3ms at 48kHz)
// EMA time constant in units of read chunks: ~96 * 5.3ms ~= 500ms. render.c
// only samples mic_get_noise_level() every 200ms anyway, so smoothing needs
// to span multiple *display* ticks, not just multiple I2S chunks, or it's
// invisible — a smaller value here reintroduces the jumpy graph.
#define NOISE_EMA_CHUNKS     96

static const char *TAG = "mic";

static i2s_chan_handle_t s_rx_chan;
static volatile uint8_t s_noise_level;
static int32_t s_smoothed_q8; // fixed-point (value * 256) EMA accumulator
static bool s_ready;

// Register writes traced 1:1 from the vendor's es7210_open()/es7210_start()/
// es7210_mic_select() (esp_codec_dev's ES7210 driver), collapsed into their
// net effect for our fixed case: MIC1+MIC3 only (this board's actual 2-mic
// array — MIC2/4 are unpopulated), slave mode (ESP32 I2S drives BCLK/WS/
// MCLK), 16-bit standard I2S, max (37.5dB) PGA gain — this board's approximate
// noise meter wants sensitivity over headroom. Reusing their whole esp_codec_dev
// framework for this one chip felt like the wrong tradeoff — see comment
// above MIC_SAMPLE_RATE_HZ for why these values aren't arbitrary.
typedef struct {
    uint8_t reg;
    uint8_t val;
} es7210_reg_t;

static const es7210_reg_t s_es7210_init_seq[] = {
    { 0x00, 0xFF }, // soft reset
    { 0x00, 0x41 },
    { 0x01, 0x3F }, // all ADC clocks off while configuring
    { 0x09, 0x30 }, // chip-state timing
    { 0x0A, 0x30 }, // power-on timing
    { 0x23, 0x2A }, // HPF quick-setup, channels 1/2
    { 0x22, 0x0A },
    { 0x20, 0x0A }, // HPF quick-setup, channels 3/4
    { 0x21, 0x2A },
    { 0x40, 0x43 }, // analog power: VDDA 3.3V, VMID 5k
    { 0x41, 0x70 }, // MIC1/2 bias 2.87V
    { 0x42, 0x70 }, // MIC3/4 bias 2.87V
    { 0x07, 0x20 }, // OSR — see MIC_SAMPLE_RATE_HZ comment
    { 0x02, 0xC1 }, // mainclk divider — see MIC_SAMPLE_RATE_HZ comment
    { 0x43, 0x00 }, { 0x44, 0x00 }, { 0x45, 0x00 }, { 0x46, 0x00 }, // clear all 4 mics' gain/enable bits
    { 0x4B, 0xFF }, { 0x4C, 0xFF },                                 // power down all 4 mics
    { 0x01, 0x20 }, // enable ADC clock for MIC1 + MIC3 only (0x3F & ~(0x0B|0x15))
    { 0x4B, 0x00 }, // power up MIC1/2 analog pair
    { 0x43, 0x1E }, // MIC1: enable (bit4) + 37.5dB gain (low nibble 0xE, chip max)
    { 0x4C, 0x00 }, // power up MIC3/4 analog pair
    { 0x45, 0x1E }, // MIC3: enable (bit4) + 37.5dB gain (chip max)
    { 0x12, 0x00 }, // 2-slot (non-TDM) output frame — only 2 mics selected
    { 0x11, 0x60 }, // 16-bit samples, standard (Philips) I2S format
    { 0x06, 0x00 }, // power-down register: powered up
    { 0x47, 0x08 }, { 0x48, 0x08 }, { 0x49, 0x08 }, { 0x4A, 0x08 }, // per-mic analog stage power
};

static esp_err_t es7210_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, sizeof(buf), 100);
}

static esp_err_t es7210_update_bits(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t mask, uint8_t data)
{
    uint8_t v = 0;
    esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, &v, 1, 100);
    if (err != ESP_OK) {
        return err;
    }
    v = (v & (uint8_t)~mask) | (data & mask);
    return es7210_write(dev, reg, v);
}

static esp_err_t es7210_bring_up(i2c_master_bus_handle_t sys_i2c_bus)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES7210_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_master_bus_add_device(sys_i2c_bus, &dev_cfg, &dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ES7210 I2C device add failed: %s", esp_err_to_name(err));
        return err;
    }

    for (size_t i = 0; i < sizeof(s_es7210_init_seq) / sizeof(s_es7210_init_seq[0]); i++) {
        err = es7210_write(dev, s_es7210_init_seq[i].reg, s_es7210_init_seq[i].val);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ES7210 reg 0x%02x write failed: %s", s_es7210_init_seq[i].reg, esp_err_to_name(err));
            return err;
        }
    }
    // Mode config reg08 bit0: 0 = I2S slave (ESP32 drives BCLK/WS/MCLK, see
    // MIC_SAMPLE_RATE_HZ). Read-modify-write since other bits' meaning here
    // isn't documented in the source this was traced from.
    err = es7210_update_bits(dev, 0x08, 0x01, 0x00);
    if (err != ESP_OK) {
        return err;
    }
    err = es7210_write(dev, 0x00, 0x71); // latch config
    if (err != ESP_OK) {
        return err;
    }
    return es7210_write(dev, 0x00, 0x41);
}

static esp_err_t mic_i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = MIC_MCLK_GPIO,
            .bclk = MIC_BCLK_GPIO,
            .ws = MIC_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_DIN_GPIO,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    err = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        return err;
    }
    return i2s_channel_enable(s_rx_chan);
}

// Runs forever once started: block on the next I2S DMA chunk, turn it into
// an RMS-based 0-100 "loudness" guess, publish it, repeat. Never routed
// through state.c's on-change notify — that path re-renders the whole
// status screen per update, way too heavy for a value that changes many
// times a second; render.c just polls mic_get_noise_level() on its own timer
// instead.
static void mic_task(void *arg)
{
    (void)arg;
    int16_t buf[NOISE_READ_FRAMES * 2]; // stereo

    for (;;) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx_chan, buf, sizeof(buf), &bytes_read, portMAX_DELAY);
        if (err != ESP_OK || bytes_read == 0) {
            continue;
        }

        size_t n = bytes_read / sizeof(int16_t);
        uint64_t sum_sq = 0;
        for (size_t i = 0; i < n; i++) {
            int32_t s = buf[i];
            sum_sq += (uint32_t)(s * s);
        }
        float rms = sqrtf((float)sum_sq / (float)n);

        uint32_t pct = (uint32_t)(rms * 100.0f / NOISE_RMS_FULL_SCALE);
        if (pct > 100) {
            pct = 100;
        }

        // Exponential smoothing in Q8 fixed point — plain integer percent
        // smoothing truncated the update to 0 whenever the gap was smaller
        // than the divisor, which stalled it instead of easing it, and made
        // the graph look just as jumpy as no smoothing at all.
        int32_t target_q8 = (int32_t)pct * 256;
        s_smoothed_q8 += (target_q8 - s_smoothed_q8) / NOISE_EMA_CHUNKS;
        int32_t level = s_smoothed_q8 / 256;
        if (level < 0) {
            level = 0;
        } else if (level > 100) {
            level = 100;
        }
        s_noise_level = (uint8_t)level;
    }
}

void mic_init(const lcd_handle_t *lcd)
{
    if (es7210_bring_up(lcd->sys_i2c_bus) != ESP_OK) {
        ESP_LOGW(TAG, "ES7210 bring-up failed, noise level will stay 0");
        return;
    }
    if (mic_i2s_init() != ESP_OK) {
        ESP_LOGW(TAG, "I2S init failed, noise level will stay 0");
        return;
    }

    if (xTaskCreate(mic_task, "mic_noise", NOISE_TASK_STACK, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Failed to create mic_task");
        return;
    }

    s_ready = true;
    ESP_LOGI(TAG, "Mic ready (approximate noise level only)");
}

uint8_t mic_get_noise_level(void)
{
    return s_ready ? s_noise_level : 0;
}
