/*
 * msc_disk.c - TinyUSB MSC device callbacks. Backing store is chosen at runtime:
 *
 *   - microSD present  -> serve the card's raw 512-byte sectors (via sdcard.c,
 *                         which links sdmmc). Fast, no caching needed.
 *   - no card          -> serve the internal "storage" wear-levelling partition
 *                         (via storage.c) through the LRU write-back cache below.
 *
 * The wl path needs the cache because CONFIG_WL_SECTOR_SIZE=512 makes every
 * wl_write an internal flash-page update; a host FAT driver interleaves data
 * with FAT/dir sectors, so a naive one-sector cache thrashes and a multi-MB copy
 * dissolves into thousands of erases. We keep dirty sectors resident and flush
 * on eviction / SCSI SYNCHRONIZE CACHE / eject. RAM = MSC_CACHE_SECTORS *
 * sector_size (64 * 512 = 32 KiB); tune with -DMSC_CACHE_SECTORS=N. The SD path
 * bypasses the cache entirely.
 *
 * Ownership: host and the on-device flash/serialflash commands are mutually
 * exclusive (storage.c's app-owns flag gates MSC readiness for the wl path).
 *
 * Firmware for ESP-IDF 5.1 / TinyUSB - not host-compilable.
 */
#include "tusb.h"
#include "storage.h"
#include "sdcard.h"
#include "wear_levelling.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>

#define MSC_MAX_SECTOR 4096U

/* Write-back cache depth (wl path only). RAM = MSC_CACHE_SECTORS * sector_size. */
#ifndef MSC_CACHE_SECTORS
#define MSC_CACHE_SECTORS 64U
#endif

/* SCSI opcode not surfaced as a macro by this TinyUSB; issued by hosts on sync. */
#ifndef SCSI_CMD_SYNCHRONIZE_CACHE_10
#define SCSI_CMD_SYNCHRONIZE_CACHE_10 0x35
#endif

/* ---- backing selection ------------------------------------------------- */

static inline bool on_sd(void) { return sdcard_present(); }

static uint32_t backing_sector_size(void)
{
	return on_sd() ? sdcard_block_size() : storage_sector_size();
}
static uint32_t backing_sector_count(void)
{
	return on_sd() ? sdcard_block_count() : storage_sector_count();
}
static bool backing_ready(void)
{
	return on_sd() || storage_wl() != WL_INVALID_HANDLE;
}

/* ---- wl write-back cache (internal-flash path) -------------------------- */

static int32_t  s_lba[MSC_CACHE_SECTORS];
static bool     s_dirty[MSC_CACHE_SECTORS];
static uint32_t s_used[MSC_CACHE_SECTORS];   /* LRU tick */
static uint8_t *s_buf;                        /* nslots * s_bs, lazily allocated */
static uint32_t s_nslots;                     /* 0 until initialised */
static uint32_t s_bs;                         /* wl sector size */
static uint32_t s_clock;
static uint8_t  s_fallback[MSC_MAX_SECTOR];   /* one-slot fallback */

static inline uint8_t *slot_ptr(uint32_t i) { return s_buf + (size_t)i * s_bs; }

static bool cache_ready(void)
{
	if (s_nslots)
		return true;
	uint32_t bs = storage_sector_size();
	if (bs == 0 || bs > MSC_MAX_SECTOR)
		return false;
	s_buf = heap_caps_malloc((size_t)MSC_CACHE_SECTORS * bs, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	if (s_buf) {
		s_nslots = MSC_CACHE_SECTORS;
	} else {
		s_buf = s_fallback;
		s_nslots = 1;
	}
	s_bs = bs;
	for (uint32_t i = 0; i < s_nslots; i++) {
		s_lba[i] = -1;
		s_dirty[i] = false;
		s_used[i] = 0;
	}
	return true;
}

static int slot_find(uint32_t lba)
{
	for (uint32_t i = 0; i < s_nslots; i++)
		if (s_lba[i] == (int32_t)lba)
			return (int)i;
	return -1;
}

static bool flush_slot(uint32_t i)
{
	if (!s_dirty[i])
		return true;
	wl_handle_t h = storage_wl();
	if (h == WL_INVALID_HANDLE)
		return false;
	const size_t addr = (size_t)s_lba[i] * s_bs;
	if (wl_erase_range(h, addr, s_bs) != ESP_OK)
		return false;
	if (wl_write(h, addr, slot_ptr(i), s_bs) != ESP_OK)
		return false;
	s_dirty[i] = false;
	return true;
}

static void flush_all(void)
{
	for (uint32_t i = 0; i < s_nslots; i++)
		flush_slot(i);
}

static int slot_bind(uint32_t lba)
{
	int hit = slot_find(lba);
	if (hit >= 0)
		return hit;
	uint32_t victim = 0, best = 0xFFFFFFFFu;
	for (uint32_t i = 0; i < s_nslots; i++) {
		if (s_lba[i] < 0) { victim = i; best = 0; break; }
		if (s_used[i] < best) { best = s_used[i]; victim = i; }
	}
	flush_slot(victim);
	s_lba[victim] = (int32_t)lba;
	s_dirty[victim] = false;
	return (int)victim;
}

/* ---- SCSI callbacks ---------------------------------------------------- */

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vid[8], uint8_t pid[16], uint8_t rev[4])
{
	(void)lun;
	memcpy(vid, "Cardputr", 8);
	memcpy(pid, on_sd() ? "microSD         " : "BMP Storage     ", 16);
	memcpy(rev, "1.0 ", 4);
}

/* Not-ready while the app holds the FS (wl path) or the backing is absent. */
bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
	(void)lun;
	if (!backing_ready() || (!on_sd() && storage_app_owns())) {
		tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x04, 0x01);
		return false;
	}
	return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
	(void)lun;
	*block_count = backing_sector_count();
	*block_size = (uint16_t)backing_sector_size();
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
	(void)lun;
	(void)power_condition;
	if (load_eject && !start && !on_sd())
		flush_all();
	return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
	(void)lun;

	if (on_sd()) {
		const uint32_t bs = sdcard_block_size();
		if (bs == 0 || lba >= sdcard_block_count() || offset + bufsize > bs)
			return -1;
		if (offset == 0 && (bufsize % bs) == 0) {
			if (sdcard_read_blocks(buffer, lba, bufsize / bs) != 0)
				return -1;
		} else {
			static uint8_t bb[MSC_MAX_SECTOR];
			if (sdcard_read_blocks(bb, lba, 1) != 0)
				return -1;
			memcpy(buffer, bb + offset, bufsize);
		}
		return (int32_t)bufsize;
	}

	if (!cache_ready())
		return -1;
	if (lba >= storage_sector_count() || offset + bufsize > s_bs)
		return -1;
	int i = slot_find(lba);
	if (i >= 0)
		memcpy(buffer, slot_ptr((uint32_t)i) + offset, bufsize);
	else if (wl_read(storage_wl(), (size_t)lba * s_bs + offset, buffer, bufsize) != ESP_OK)
		return -1;
	return (int32_t)bufsize;
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
	(void)lun;
	return true;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
	(void)lun;

	if (on_sd()) {
		const uint32_t bs = sdcard_block_size();
		if (bs == 0 || lba >= sdcard_block_count() || offset + bufsize > bs)
			return -1;
		if (offset == 0 && (bufsize % bs) == 0) {
			if (sdcard_write_blocks(buffer, lba, bufsize / bs) != 0)
				return -1;
		} else {
			static uint8_t bb[MSC_MAX_SECTOR];
			if (sdcard_read_blocks(bb, lba, 1) != 0)
				return -1;
			memcpy(bb + offset, buffer, bufsize);
			if (sdcard_write_blocks(bb, lba, 1) != 0)
				return -1;
		}
		return (int32_t)bufsize;
	}

	if (!cache_ready())
		return -1;
	if (lba >= storage_sector_count() || offset + bufsize > s_bs)
		return -1;
	int i = slot_find(lba);
	if (i < 0) {
		i = slot_bind(lba);
		if (!(offset == 0 && bufsize == s_bs)) {
			if (wl_read(storage_wl(), (size_t)lba * s_bs, slot_ptr((uint32_t)i), s_bs) != ESP_OK) {
				s_lba[i] = -1;
				return -1;
			}
		}
	}
	memcpy(slot_ptr((uint32_t)i) + offset, buffer, bufsize);
	s_dirty[i] = true;
	s_used[i] = ++s_clock;
	return (int32_t)bufsize;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
	(void)lun;
	(void)buffer;
	(void)bufsize;
	switch (scsi_cmd[0]) {
	case SCSI_CMD_SYNCHRONIZE_CACHE_10:
		if (!on_sd())
			flush_all();   /* SD writes are synchronous already */
		return 0;
	case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
		return 0;
	default:
		tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
		return -1;
	}
}
