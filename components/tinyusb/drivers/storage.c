/*
 * storage.c - owns the internal "storage" wear-levelling FAT partition and
 * arbitrates ownership between USB-MSC (host) and the on-device filesystem.
 *
 * NOTE: this is firmware for ESP-IDF 5.1 and cannot be compiled on the host.
 * The FatFS-on-existing-wl-handle registration mirrors what
 * esp_vfs_fat_spiflash_mount_rw_wl() does internally; if your IDF point release
 * differs, cross-check against components/fatfs/vfs/vfs_fat_spiflash.c.
 */
#include "storage.h"
#include <string.h>
#include <inttypes.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_vfs_fat.h>
#include "diskio_impl.h"
#include "diskio_wl.h"
#include "ff.h"

static const char *TAG = "storage";

static wl_handle_t s_wl = WL_INVALID_HANDLE;
static uint32_t s_sector_size;
static uint32_t s_sector_count;

/* FatFS state while the app owns the disk. */
static FATFS *s_fs;
static BYTE s_pdrv = 0xFF;
static volatile bool s_app_owns;

/* Static state for the one-shot format performed at init. */
static FATFS s_fmt_fs;
static uint8_t s_fmt_work[4096];

bool storage_init(const char *partition_label)
{
	if (s_wl != WL_INVALID_HANDLE)
		return true;
	const esp_partition_t *part = esp_partition_find_first(
		ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, partition_label);
	if (!part) {
		ESP_LOGE(TAG, "partition '%s' not found (add it to partitions.csv)", partition_label);
		return false;
	}

	/* Single persistent wl mount - this handle is what USB-MSC serves raw
	 * blocks from. (Do NOT also mount via esp_vfs_fat_spiflash_mount_rw_wl:
	 * that takes a second wl instance and leaves this one unable to mount.) */
	if (wl_mount(part, &s_wl) != ESP_OK) {
		ESP_LOGE(TAG, "wl_mount failed");
		s_wl = WL_INVALID_HANDLE;
		return false;
	}
	s_sector_size = (uint32_t)wl_sector_size(s_wl);
	s_sector_count = (uint32_t)(wl_size(s_wl) / s_sector_size);

	/* Format-if-blank through our own handle, so USB-MSC exposes a mountable
	 * FAT (super-floppy, no MBR - matches what hosts expect for a raw volume).
	 * Existing content survives: we only mkfs when there is no filesystem. */
	BYTE pdrv = 0xFF;
	if (ff_diskio_get_drive(&pdrv) == ESP_OK && pdrv != 0xFF) {
		ff_diskio_register_wl_partition(pdrv, s_wl);
		char drv[3] = {(char)('0' + pdrv), ':', 0};
		FRESULT fr = f_mount(&s_fmt_fs, drv, 1);
		if (fr == FR_NO_FILESYSTEM) {
			const MKFS_PARM opt = {.fmt = FM_FAT | FM_SFD};
			fr = f_mkfs(drv, &opt, s_fmt_work, sizeof(s_fmt_work));
			if (fr == FR_OK)
				ESP_LOGI(TAG, "formatted '%s' (empty FAT)", partition_label);
			else
				ESP_LOGE(TAG, "f_mkfs failed: %d", fr);
		}
		f_mount(NULL, drv, 0);
		ff_diskio_register(pdrv, NULL); /* release drive, keep s_wl mounted */
	} else {
		ESP_LOGW(TAG, "no free FatFS drive to format-check storage");
	}

	ESP_LOGI(TAG, "storage: %" PRIu32 " sectors x %" PRIu32 " bytes", s_sector_count, s_sector_size);
	return true;
}

wl_handle_t storage_wl(void) { return s_wl; }
uint32_t storage_sector_size(void) { return s_sector_size; }
uint32_t storage_sector_count(void) { return s_sector_count; }
bool storage_app_owns(void) { return s_app_owns; }

/* Mount FatFS onto the already-mounted wl handle and take it from the host. */
bool storage_acquire(const char *base_path)
{
	if (s_wl == WL_INVALID_HANDLE)
		return false;
	if (s_app_owns)
		return true;

	/* Signal MSC to report not-ready so the host stops touching the disk. */
	s_app_owns = true;

	if (ff_diskio_get_drive(&s_pdrv) != ESP_OK || s_pdrv == 0xFF) {
		ESP_LOGE(TAG, "no free FatFS drive");
		s_app_owns = false;
		return false;
	}
	char drv[3] = {(char)('0' + s_pdrv), ':', 0};

	esp_err_t err = esp_vfs_fat_register(base_path, drv, 4, &s_fs);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "vfs_fat_register: %s", esp_err_to_name(err));
		ff_diskio_register(s_pdrv, NULL); /* release the drive slot */
		s_pdrv = 0xFF;
		s_app_owns = false;
		return false;
	}
	ff_diskio_register_wl_partition(s_pdrv, s_wl);

	FRESULT fr = f_mount(s_fs, drv, 1);
	if (fr != FR_OK) {
		ESP_LOGE(TAG, "f_mount: %d", fr);
		esp_vfs_fat_unregister_path(base_path);
		ff_diskio_register(s_pdrv, NULL); /* release the drive slot */
		s_pdrv = 0xFF;
		s_fs = NULL;
		s_app_owns = false;
		return false;
	}
	ESP_LOGI(TAG, "app mounted %s (host paused)", base_path);
	return true;
}

void storage_release(const char *base_path)
{
	if (!s_app_owns)
		return;
	if (s_pdrv != 0xFF) {
		char drv[3] = {(char)('0' + s_pdrv), ':', 0};
		f_mount(NULL, drv, 0);
		ff_diskio_register(s_pdrv, NULL); /* release the drive slot */
		s_pdrv = 0xFF;
	}
	esp_vfs_fat_unregister_path(base_path);
	s_fs = NULL;
	s_app_owns = false;
	ESP_LOGI(TAG, "released %s to host", base_path);
}
