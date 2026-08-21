#include "sd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd";

// Waveshare ESP32-S3-Touch-LCD-3.49: 1-bit SDMMC, from their own
// Examples/ESP-IDF/04_SD_Card (sdcard_bsp.c) — not from the schematic,
// and not the SPI MOSI/SCK/MISO + expander-CS layout used on the 7".
#define SDMMC_D0_PIN   GPIO_NUM_40
#define SDMMC_CLK_PIN  GPIO_NUM_41
#define SDMMC_CMD_PIN  GPIO_NUM_39
#define SD_MOUNT_POINT "/sdcard"

// Compressed wallpaper PNG of 640×172 is typically tens of KB. Anything
// near a megabyte is a mistaken file, not a screen asset — reject before
// the alloc can fight LVGL/BLE for heap.
#define SD_FILE_MAX_BYTES (1024 * 1024)

static sdmmc_card_t *s_card;
static bool s_mounted;

bool sd_mount(void)
{
    if (s_mounted) {
        return true;
    }

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk = SDMMC_CLK_PIN;
    slot_config.cmd = SDMMC_CMD_PIN;
    slot_config.d0 = SDMMC_D0_PIN;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config,
                                            &mount_config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mount failed (%s) — wallpapers skipped", esp_err_to_name(err));
        s_card = NULL;
        return false;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "mounted %s", SD_MOUNT_POINT);
    return true;
}

uint8_t *sd_read_file(const char *name, size_t *out_size)
{
    if (out_size) {
        *out_size = 0;
    }
    if (!s_mounted || name == NULL || name[0] == '\0' || strchr(name, '/') != NULL) {
        return NULL;
    }

    char path[64];
    int n = snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, name);
    if (n < 0 || n >= (int)sizeof(path)) {
        return NULL;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGI(TAG, "%s not found — skip", name);
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz <= 0 || sz > (long)SD_FILE_MAX_BYTES) {
        ESP_LOGW(TAG, "%s size %ld rejected (max %d)", name, sz, SD_FILE_MAX_BYTES);
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    uint8_t *buf = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = malloc((size_t)sz);
    }
    if (buf == NULL) {
        ESP_LOGW(TAG, "%s: out of memory (%ld bytes)", name, sz);
        fclose(f);
        return NULL;
    }

    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        ESP_LOGW(TAG, "%s: short read %u/%ld", name, (unsigned)got, sz);
        free(buf);
        return NULL;
    }

    if (out_size) {
        *out_size = (size_t)sz;
    }
    ESP_LOGI(TAG, "read %s (%ld bytes)", name, sz);
    return buf;
}

void sd_unmount(void)
{
    if (!s_mounted) {
        return;
    }
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    s_card = NULL;
    s_mounted = false;
    ESP_LOGI(TAG, "unmounted");
}
