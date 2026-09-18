// Standalone serial (UART ROM-bootloader) flasher for the Cardputer BMP.
// Flashes an Espressif SoC over the Grove UART via espressif/esp-serial-flasher,
// streaming the image from /sdcard. Parallels the SWD `flash` verb.

#include "driver/uart.h"
#include "serial_flash.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_loader.h"

// Port header for esp-serial-flasher on ESP-IDF targets
#if __has_include("esp32_port.h")
    #include "esp32_port.h"
#elif __has_include("loader_port.h")
    #include "loader_port.h"
#endif

#include "ui.h"           // ui_capture_write()
#include "platform.h"     // TMS_PIN=G1/GPIO1, TCK_PIN=G2/GPIO2
#include "storage.h"      // storage_acquire()/storage_release()

#define SER_UART_NUM    UART_NUM_1
#define SER_TX_PIN      TMS_PIN       // G1/GPIO1 -> target RX
#define SER_RX_PIN      TCK_PIN       // G2/GPIO2 <- target TX
#define SER_RESET_PIN   GPIO_NUM_NC   // no spare Grove pin; set if wired
#define SER_BOOT_PIN    GPIO_NUM_NC   // no spare Grove pin; set if wired
#define SER_INIT_BAUD   115200
#define SER_FAST_BAUD   460800
#define SER_BLOCK       1024
#define SER_DEFAULT_OFF 0x10000u      // app partition on most ESP targets

static void slog(const char *fmt, ...)
{
	char b[128];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(b, sizeof b - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	size_t len = ((size_t)n < sizeof b - 1) ? (size_t)n : sizeof b - 2;
	b[len] = '\n';
	b[len + 1] = '\0';
	ui_capture_write(b);
}

static const char *sdpath(const char *in, char *buf, size_t n)
{
	if (in[0] == '/')
		return in;
	snprintf(buf, n, "/sdcard/%s", in);
	return buf;
}

// Return G1/G2 to plain GPIO so a later swd_scan can drive them again.
static void release_grove_pins(void)
{
	gpio_reset_pin((gpio_num_t)SER_TX_PIN);
	gpio_reset_pin((gpio_num_t)SER_RX_PIN);
}

bool serial_flash_cmd(int argc, char **argv)
{
	if (argc < 2) {
		slog("usage: serialflash <file> [hexoff]");
		slog("  default off 0x%05lx", (unsigned long)SER_DEFAULT_OFF);
		slog("  put target in download mode first");
		return false;
	}
	uint32_t offset = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 16) : SER_DEFAULT_OFF;
	if (offset & 3u) {
		slog("offset must be 4-byte aligned");
		return false;
	}

	char pathbuf[128];
	const char *path = sdpath(argv[1], pathbuf, sizeof pathbuf);

	bool have_fs = storage_acquire("/sdcard");
	FILE *f = fopen(path, "rb");
	if (!f) {
		slog("open %s failed", path);
		if (have_fs)
			storage_release("/sdcard");
		return false;
	}
	fseek(f, 0, SEEK_END);
	long fsz = ftell(f);
	rewind(f);
	if (fsz <= 0) {
		slog("empty/bad file");
		fclose(f);
		if (have_fs)
			storage_release("/sdcard");
		return false;
	}
	uint32_t image_size = (uint32_t)((fsz + 3) & ~3L); // pad up to 4 bytes

    // 1. Hardware Port Initialization using loader_esp32_config_t
	loader_esp32_config_t config = {
		.baud_rate = SER_INIT_BAUD,
		.uart_num = SER_UART_NUM,
		.uart_tx_pin = SER_TX_PIN,
		.uart_rx_pin = SER_RX_PIN,
		.reset_trigger_pin = SER_RESET_PIN,
		.gpio0_trigger_pin = SER_BOOT_PIN,
	};

	if (loader_port_esp32_init(&config) != ESP_LOADER_SUCCESS) {
		slog("uart init failed");
		fclose(f);
		if (have_fs)
			storage_release("/sdcard");
		return false;
	}

	bool ok = false;

	// 2. Connect to Target
	esp_loader_connect_args_t ca = ESP_LOADER_CONNECT_DEFAULT();
	if (esp_loader_connect(&ca) != ESP_LOADER_SUCCESS) {
		slog("connect failed");
		slog("hold BOOT/IO0 low, pulse EN, retry");
		goto done;
	}
	slog("connected: chip %d", (int)esp_loader_get_target());

	// 3. Change Transmission Rate (Baud Rate)
	if (esp_loader_change_transmission_rate(SER_FAST_BAUD) == ESP_LOADER_SUCCESS)
		slog("baud -> %d", SER_FAST_BAUD);

	// 4. Start Flash Process
	if (esp_loader_flash_start(offset, image_size, SER_BLOCK) != ESP_LOADER_SUCCESS) {
		slog("flash_start failed");
		goto done;
	}
	slog("erased, prog 0x%08lx +%lu", (unsigned long)offset, (unsigned long)image_size);

	// 5. Stream and Write Payload
	static uint8_t buf[SER_BLOCK];
	uint32_t sent = 0, next = 16384;
	while (sent < image_size) {
		size_t want = image_size - sent;
		if (want > SER_BLOCK)
			want = SER_BLOCK;
		size_t got = fread(buf, 1, want, f);
		if (got < want)
			memset(buf + got, 0xff, want - got); // pad tail

		if (esp_loader_flash_write(buf, want) != ESP_LOADER_SUCCESS) {
			slog("write failed @%lu", (unsigned long)sent);
			goto done;
		}
		sent += want;
		if (sent >= next && sent < image_size) {
			slog("  %lu / %lu", (unsigned long)sent, (unsigned long)image_size);
			next += 16384;
		}
	}

	// 6. Finish and Reset
	bool reboot_target = true;
	if (esp_loader_flash_finish(reboot_target) != ESP_LOADER_SUCCESS) {
		slog("verify/finish failed");
		goto done;
	}
	esp_loader_reset_target(); // no-op if RESET pin is NC
	slog("serialflash ok: %lu bytes @0x%08lx", (unsigned long)image_size, (unsigned long)offset);
	ok = true;

done:
	// 7. Cleanup & Release
	loader_port_esp32_deinit();
	release_grove_pins();       // hand G1/G2 back for SWD
	fclose(f);
	if (have_fs)
		storage_release("/sdcard");
	if (!ok)
		slog("run swd_scan again before SWD use");
	return ok;
}
