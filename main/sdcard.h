#pragma once
#include <stdbool.h>

/*
 * Mounts the Cardputer microSD at /sdcard so `flash <path>` can read images.
 *
 * >>> CONFIRM THESE PINS AND THE BUS AGAINST YOUR BOARD BEFORE USE. <<<
 *
 * Two things to check on the Cardputer:
 *   1. Pin numbers. The values in sdcard.c are the commonly documented M5Stack
 *      Cardputer microSD SPI pins, but verify against your unit's schematic.
 *   2. Bus sharing with the ST7789. Your display is on an SPI host already
 *      (your notes say SPI3_HOST). If the SD shares that bus, do NOT call
 *      spi_bus_initialize() again for it: initialize the bus once, then mount
 *      the SD as a device on the already-initialized host with its own CS.
 *      sdcard.c has a SDCARD_SHARED_BUS switch for exactly this.
 */

bool sdcard_mount(void);   // returns true on success, mounts at /sdcard
void sdcard_unmount(void);
