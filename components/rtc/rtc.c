#include "rtc.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "driver/i2c_master.h"
#include "esp_log.h"

// Waveshare ESP32-S3-Touch-LCD-3.49: PCF85063 on the same system I2C bus as
// the TCA9554 (lcd.c). Address and register map verified against Waveshare's
// 02_I2C_PCF85063 example (user_config.h RTC_PCF85063_ADDR + SensorLib
// SensorPCF85063) — not derived from the schematic. We talk to the chip
// directly rather than pulling in SensorLib: same approach as mic.c / ES7210.
#define PCF85063_ADDR       0x51
#define PCF85063_CTRL1      0x00
#define PCF85063_SEC        0x04

#define CTRL1_STOP          (1u << 5)
#define CTRL1_12_24         (1u << 1)

#define I2C_TIMEOUT_MS      100
#define TZ_OFFSET_MAX_SEC   (18 * 3600)

static const char *TAG = "rtc";

static i2c_master_dev_handle_t s_dev;
static bool s_chip_ok;
static bool s_time_valid;

static uint8_t dec2bcd(uint8_t v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static uint8_t bcd2dec(uint8_t v)
{
    return (uint8_t)((v >> 4) * 10 + (v & 0x0f));
}

// Sakamoto: 0 = Sunday, matching the PCF85063 weekday encoding.
static uint8_t day_of_week(uint16_t y, uint8_t m, uint8_t d)
{
    static const uint8_t t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) {
        y--;
    }
    return (uint8_t)((y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7);
}

static esp_err_t rtc_write(uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[8];
    if (len + 1 > sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = reg;
    memcpy(&buf[1], data, len);
    return i2c_master_transmit(s_dev, buf, len + 1, I2C_TIMEOUT_MS);
}

static esp_err_t rtc_read(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, I2C_TIMEOUT_MS);
}

static void apply_system_time(time_t local_sec)
{
    struct timeval tv = {
        .tv_sec = local_sec,
        .tv_usec = 0,
    };
    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGW(TAG, "settimeofday failed");
        return;
    }
    s_time_valid = true;
}

// mktime() follows TZ; force UTC so calendar fields map 1:1 onto time_t.
static time_t timegm_utc(struct tm *t)
{
    setenv("TZ", "UTC0", 1);
    tzset();
    return mktime(t);
}

static esp_err_t chip_write_local(const struct tm *t)
{
    if (!s_chip_ok) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t ctrl1 = 0;
    esp_err_t err = rtc_read(PCF85063_CTRL1, &ctrl1, 1);
    if (err != ESP_OK) {
        return err;
    }
    ctrl1 &= (uint8_t)~CTRL1_12_24;
    ctrl1 |= CTRL1_STOP;
    err = rtc_write(PCF85063_CTRL1, &ctrl1, 1);
    if (err != ESP_OK) {
        return err;
    }

    uint16_t year = (uint16_t)(t->tm_year + 1900);
    uint8_t month = (uint8_t)(t->tm_mon + 1);
    uint8_t regs[7] = {
        (uint8_t)(dec2bcd((uint8_t)t->tm_sec) & 0x7f),
        dec2bcd((uint8_t)t->tm_min),
        dec2bcd((uint8_t)t->tm_hour),
        dec2bcd((uint8_t)t->tm_mday),
        day_of_week(year, month, (uint8_t)t->tm_mday),
        dec2bcd(month),
        dec2bcd((uint8_t)(year % 100)),
    };
    err = rtc_write(PCF85063_SEC, regs, sizeof(regs));
    if (err != ESP_OK) {
        return err;
    }

    ctrl1 &= (uint8_t)~CTRL1_STOP;
    return rtc_write(PCF85063_CTRL1, &ctrl1, 1);
}

static esp_err_t chip_read_local(struct tm *out)
{
    uint8_t regs[7];
    esp_err_t err = rtc_read(PCF85063_SEC, regs, sizeof(regs));
    if (err != ESP_OK) {
        return err;
    }
    if (regs[0] & 0x80) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(out, 0, sizeof(*out));
    out->tm_sec = bcd2dec(regs[0] & 0x7f);
    out->tm_min = bcd2dec(regs[1] & 0x7f);
    out->tm_hour = bcd2dec(regs[2] & 0x3f);
    out->tm_mday = bcd2dec(regs[3] & 0x3f);
    out->tm_mon = (int)bcd2dec(regs[5] & 0x1f) - 1;
    out->tm_year = (int)bcd2dec(regs[6]) + 2000 - 1900;
    out->tm_isdst = 0;
    return ESP_OK;
}

void rtc_clock_init(const lcd_handle_t *lcd)
{
    if (!lcd || !lcd->sys_i2c_bus) {
        ESP_LOGW(TAG, "no system I2C bus, clock stays unset");
        return;
    }

    esp_err_t err = i2c_master_probe(lcd->sys_i2c_bus, PCF85063_ADDR, I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 probe failed: %s", esp_err_to_name(err));
        return;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF85063_ADDR,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(lcd->sys_i2c_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 add device failed: %s", esp_err_to_name(err));
        return;
    }
    s_chip_ok = true;

    uint8_t ctrl1 = 0;
    err = rtc_read(PCF85063_CTRL1, &ctrl1, 1);
    if (err == ESP_OK) {
        uint8_t next = (uint8_t)((ctrl1 & (uint8_t)~CTRL1_12_24) & (uint8_t)~CTRL1_STOP);
        if (next != ctrl1) {
            (void)rtc_write(PCF85063_CTRL1, &next, 1);
        }
    }

    struct tm t;
    err = chip_read_local(&t);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "PCF85063 present, time not valid yet (waiting for BLE)");
        return;
    }

    time_t local = timegm_utc(&t);
    if (local == (time_t)-1) {
        ESP_LOGW(TAG, "mktime failed on chip calendar");
        return;
    }
    apply_system_time(local);
    ESP_LOGI(TAG, "loaded %04d-%02d-%02d %02d:%02d:%02d from PCF85063",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
}

esp_err_t rtc_clock_set_unix(int64_t unix_utc, int32_t tz_offset_sec)
{
    if (unix_utc < 946684800LL || unix_utc > 4102444799LL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (tz_offset_sec > TZ_OFFSET_MAX_SEC || tz_offset_sec < -TZ_OFFSET_MAX_SEC) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t local64 = unix_utc + tz_offset_sec;
    if (local64 < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    time_t local = (time_t)local64;

    struct tm t;
    if (gmtime_r(&local, &t) == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int year = t.tm_year + 1900;
    if (year < 2000 || year > 2099) {
        return ESP_ERR_INVALID_ARG;
    }

    apply_system_time(local);

    if (s_chip_ok) {
        esp_err_t err = chip_write_local(&t);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "chip write failed: %s (ESP clock still set)",
                     esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "set %04d-%02d-%02d %02d:%02d:%02d (unix=%lld tz=%ld)",
             year, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec,
             (long long)unix_utc, (long)tz_offset_sec);
    return ESP_OK;
}

bool rtc_clock_get_hms(uint8_t *hour, uint8_t *minute, uint8_t *second)
{
    if (!s_time_valid) {
        return false;
    }

    time_t now = time(NULL);
    struct tm t;
    if (gmtime_r(&now, &t) == NULL) {
        return false;
    }
    if (hour) {
        *hour = (uint8_t)t.tm_hour;
    }
    if (minute) {
        *minute = (uint8_t)t.tm_min;
    }
    if (second) {
        *second = (uint8_t)t.tm_sec;
    }
    return true;
}
