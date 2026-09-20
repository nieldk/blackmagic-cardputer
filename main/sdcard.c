#include "sdcard.h"
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <driver/sdspi_host.h>
#include <driver/spi_common.h>
static const char *TAG = "sdcard";

static sdmmc_card_t *s_card;

// >>> CONFIRM THESE. Documented M5Stack Cardputer microSD SPI pins. <<<
#define SD_PIN_CLK  40
#define SD_PIN_MISO 39
#define SD_PIN_MOSI 14
#define SD_PIN_CS   12
// The SD and the ST7789 share SPI3_HOST on the Cardputer. If the display driver
// already ran spi_bus_initialize(SPI3_HOST) (it does), set SDCARD_SHARED_BUS=1
// so we only add the SD as a device on that bus instead of initialising it
// again (which would fail with ESP_ERR_INVALID_STATE and drop the card).
#define SD_SPI_HOST       SPI3_HOST
#define SDCARD_SHARED_BUS 1
#define MOUNT_POINT "/sdcard"

// Mount a physically-present microSD. No internal-flash fallback: when there is
// no card, storage.c's wl partition already backs both USB-MSC and on-device
// /sdcard access (via storage_acquire), so a fallback here would double-mount
// the same partition.
bool sdcard_mount(void)
{
	if (s_card) // already mounted
		return true;
	esp_err_t err;
#if !SDCARD_SHARED_BUS
	spi_bus_config_t bus = {
		.mosi_io_num = SD_PIN_MOSI,
		.miso_io_num = SD_PIN_MISO,
		.sclk_io_num = SD_PIN_CLK,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = 4096,
	};
	err = spi_bus_initialize(SD_SPI_HOST, &bus, SDSPI_DEFAULT_DMA);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
		return false;
	}
#endif
	sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
	slot.gpio_cs = SD_PIN_CS;
	slot.host_id = SD_SPI_HOST;
	sdmmc_host_t host = SDSPI_HOST_DEFAULT();
	host.slot = SD_SPI_HOST;
	esp_vfs_fat_sdmmc_mount_config_t mcfg = {
		.format_if_mount_failed = false,
		.max_files = 4,
		.allocation_unit_size = 16 * 1024,
	};
	err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot, &mcfg, &s_card);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "no SD card (%s); using internal storage", esp_err_to_name(err));
		s_card = NULL;
#if !SDCARD_SHARED_BUS
		spi_bus_free(SD_SPI_HOST);
#endif
		return false;
	}
	ESP_LOGI(TAG, "mounted microSD at %s (%llu MB)",
	         MOUNT_POINT, ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) >> 20);
	return true;
}

void sdcard_unmount(void)
{
	if (s_card) {
		esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
		s_card = NULL;
#if !SDCARD_SHARED_BUS
		spi_bus_free(SD_SPI_HOST);
#endif
	}
}

bool sdcard_present(void) { return s_card != NULL; }

uint32_t sdcard_block_size(void)  { return s_card ? (uint32_t)s_card->csd.sector_size : 0; }
uint32_t sdcard_block_count(void) { return s_card ? (uint32_t)s_card->csd.capacity : 0; }

int sdcard_read_blocks(void *dst, uint32_t lba, uint32_t cnt)
{
	return s_card ? sdmmc_read_sectors(s_card, dst, lba, cnt) : -1;
}

int sdcard_write_blocks(const void *src, uint32_t lba, uint32_t cnt)
{
	return s_card ? sdmmc_write_sectors(s_card, src, lba, cnt) : -1;
}
