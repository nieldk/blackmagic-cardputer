/*
 * msc_disk.c - TinyUSB MSC device callbacks bridging the host to the internal
 * "storage" wear-levelling partition (via storage.c). Presents the wl sector
 * size as the logical block size, which matches how the FAT was formatted.
 *
 * These are the standard tud_msc_* callbacks; they work with vanilla TinyUSB.
 * If you instead use espressif's esp_tinyusb `tusb_msc_storage` helper, it
 * provides equivalents and you would NOT compile this file.
 *
 * Firmware for ESP-IDF 5.1 / TinyUSB - not host-compilable.
 */
#include "tusb.h"
#include "storage.h"
#include "wear_levelling.h"
#include <string.h>

/* One-sector read-modify-write buffer (wl sector, typically 4096 bytes). */
#define MSC_MAX_SECTOR 4096U
static uint8_t s_secbuf[MSC_MAX_SECTOR];
static int32_t s_secbuf_lba = -1;

static void flush_sector(void)
{
	if (s_secbuf_lba < 0)
		return;
	const uint32_t bs = storage_sector_size();
	const size_t addr = (size_t)s_secbuf_lba * bs;
	wl_erase_range(storage_wl(), addr, bs);
	wl_write(storage_wl(), addr, s_secbuf, bs);
	s_secbuf_lba = -1;
}

/* SCSI inquiry: vendor(8) / product(16) / revision(4), space-padded. */
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vid[8], uint8_t pid[16], uint8_t rev[4])
{
	(void)lun;
	memcpy(vid, "Cardputr", 8);
	memcpy(pid, "BMP Storage     ", 16);
	memcpy(rev, "1.0 ", 4);
}

/* Report not-ready while the app holds the filesystem, so the host waits. */
bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
	(void)lun;
	if (storage_wl() == WL_INVALID_HANDLE || storage_app_owns()) {
		/* 0x02 NOT READY, 0x04 LOGICAL UNIT NOT READY / BECOMING READY */
		tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x04, 0x01);
		return false;
	}
	return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
	(void)lun;
	*block_count = storage_sector_count();
	*block_size = (uint16_t)storage_sector_size();
}

/* Handle host eject: flush and stop presenting the medium. */
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
	(void)lun;
	(void)power_condition;
	if (load_eject && !start)
		flush_sector();
	return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
	(void)lun;
	const uint32_t bs = storage_sector_size();
	if (lba >= storage_sector_count() || offset + bufsize > bs)
		return -1;
	/* If the requested sector is dirty in our buffer, serve from it. */
	if ((int32_t)lba == s_secbuf_lba)
		memcpy(buffer, s_secbuf + offset, bufsize);
	else if (wl_read(storage_wl(), (size_t)lba * bs + offset, buffer, bufsize) != ESP_OK)
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
	const uint32_t bs = storage_sector_size();
	if (lba >= storage_sector_count() || offset + bufsize > bs || bs > MSC_MAX_SECTOR)
		return -1;
	/* Read-modify-write: load the target sector once, patch, flush on completion. */
	if ((int32_t)lba != s_secbuf_lba) {
		flush_sector();
		if (wl_read(storage_wl(), (size_t)lba * bs, s_secbuf, bs) != ESP_OK)
			return -1;
		s_secbuf_lba = (int32_t)lba;
	}
	memcpy(s_secbuf + offset, buffer, bufsize);
	if (offset + bufsize >= bs)
		flush_sector();
	return (int32_t)bufsize;
}

/* Minimal SCSI passthrough: reject anything not handled by TinyUSB directly. */
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
	(void)lun;
	(void)buffer;
	(void)bufsize;
	switch (scsi_cmd[0]) {
	case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
		return 0; /* accept, no data */
	default:
		tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
		return -1;
	}
}
