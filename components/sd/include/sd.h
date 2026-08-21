#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Mount the onboard TF slot as FAT at /sdcard.
 *
 * Best-effort: missing card, wiring, or filesystem logs a warning and
 * returns false — boot continues. Never formats the card.
 *
 * Pins are the Waveshare 1-bit SDMMC set (CLK 41, CMD 39, D0 40), not the
 * SPI layout from their 7" boards. See docs/KNOWLEDGE_BASE.md (sd).
 */
bool sd_mount(void);

/**
 * @brief Read a file from the FAT root into a heap buffer.
 *
 * @param name  Root filename only (`bg.png`), not a path.
 * @param out_size  Byte length of the returned buffer; 0 if NULL is returned.
 * @return Heap pointer the caller must `free()`, or NULL if the file is
 *         missing, too large, or unreadable. Missing files are not errors.
 */
uint8_t *sd_read_file(const char *name, size_t *out_size);

/**
 * @brief Unmount FAT and release the SDMMC host/DMA. Safe if not mounted.
 */
void sd_unmount(void);

#ifdef __cplusplus
}
#endif
