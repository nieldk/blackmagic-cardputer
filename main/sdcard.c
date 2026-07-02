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

// Which SPI host the SD lives on. If it shares the bus with your ST7789
// (your display is on SPI3_HOST per your notes), set SDCARD_SHARED_BUS to 1
// and make SD_SPI_HOST match the host the display already initialized. When
// shared, we skip spi_bus_initialize() and only add the SD as a bus device.
#define SD_SPI_HOST      SPI3_HOST
#define SDCARD_SHARED_BUS 0

#define MOUNT_POINT "/sdcard"

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
		ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
		s_card = NULL;
#if !SDCARD_SHARED_BUS
		spi_bus_free(SD_SPI_HOST);
#endif
		return false;
	}

	ESP_LOGI(TAG, "mounted %s", MOUNT_POINT);
	return true;
}

void sdcard_unmount(void)
{
	if (!s_card)
		return;
	esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
	s_card = NULL;
#if !SDCARD_SHARED_BUS
	spi_bus_free(SD_SPI_HOST);
#endif
}
