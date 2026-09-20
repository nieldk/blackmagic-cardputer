#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * microSD at /sdcard, plus raw block access so USB-MSC can expose the card.
 *
 * >>> CONFIRM THESE PINS AND THE BUS AGAINST YOUR BOARD BEFORE USE. <<<
 *
 * Two things to check on the Cardputer:
 *   1. Pin numbers. The values in sdcard.c are the commonly documented M5Stack
 *      Cardputer microSD SPI pins, but verify against your unit's schematic.
 *   2. Bus sharing with the ST7789. The display is on SPI3_HOST already. If the
 *      SD shares that bus, set SDCARD_SHARED_BUS=1 in sdcard.c so it does NOT
 *      call spi_bus_initialize() again but mounts the SD as a device on the
 *      already-initialized host with its own CS.
 */

/* Mount a physically-present microSD at /sdcard. Returns false if no card is
 * present (no internal-flash fallback here - storage.c owns that). */
bool sdcard_mount(void);
void sdcard_unmount(void);

/* True once a real card is mounted. USB-MSC uses this to pick the SD as its
 * backing store; when false it serves the internal "storage" partition. */
bool sdcard_present(void);

/* Raw 512-byte-sector block access for USB-MSC (thin wrappers over sdmmc_*).
 * Return 0 (ESP_OK) on success. cnt sectors starting at lba. */
int      sdcard_read_blocks(void *dst, uint32_t lba, uint32_t cnt);
int      sdcard_write_blocks(const void *src, uint32_t lba, uint32_t cnt);
uint32_t sdcard_block_size(void);    /* bytes per sector (typically 512) */
uint32_t sdcard_block_count(void);   /* total sectors on the card */
