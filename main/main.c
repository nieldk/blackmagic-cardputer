#include <stdint.h>
#include <stdio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <rom/ets_sys.h>
#include <rom/gpio.h>
#include <driver/gpio.h>

#include "usb.h"
#include "led.h"
#include "gdb_main.h"
#include "gdb_packet.h"
#include "align.h"
#include "platform.h"
#include "gdb-glue.h"
#include "soft-uart-log.h"
#include "display.h"

#include "usb-uart.h"

#include "target_lock.h"
#include "sdcard.h"
#include "storage.h"
#include <nvs_flash.h>

#if defined(CONFIG_BOARD_CARDPUTER)
#include "ui.h"
#endif

static const char* TAG = "main";

#if defined(CONFIG_BOARD_TDISPLAY_S3_AMOLED)
#define LOG_TX_PIN (40)
#else
#define LOG_TX_PIN (-1)
#endif

static char BMD_ALIGN_DEF(8) pbuf[GDB_PACKET_BUFFER_SIZE + 1U];

void gdb_application_thread(void* pvParameters) {
    ESP_LOGI("gdb", "start");
    while(1) {
        size_t size = gdb_getpacket(pbuf, GDB_PACKET_BUFFER_SIZE);
        gdb_main(pbuf, GDB_PACKET_BUFFER_SIZE, size);
    }
    ESP_LOGI("gdb", "end");
}

void pins_init() {
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (
        ((uint64_t)1 << SWCLK_PIN)
        | ((uint64_t)1 << SWDIO_PIN)
#if TDO_PIN != -1
        | ((uint64_t)1 << TDO_PIN)
#endif
#if NRST_PIN != -1
        | ((uint64_t)1 << NRST_PIN)
#endif
    );
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);

#if TDI_PIN != -1
    gpio_set_direction(TDI_PIN, GPIO_MODE_INPUT);
#endif
}

void app_main(void)
{
#if LOG_TX_PIN != -1
    soft_uart_log_init(LOG_TX_PIN, 115200);
#endif

    ESP_LOGI(TAG, "start");

    esp_err_t nvs_err = nvs_flash_init();
    if(nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    gdb_glue_init();

    display_init();          // claims SPI3_HOST for the ST7789

    led_init();
    led_set_green(255);

    // Auto-mount a microSD if present. Runs AFTER display_init (so the SD's own
    // SPI2 bus is free of the display's SPI3) and BEFORE usb_init (so USB-MSC
    // can expose the card). If no card, /sdcard falls back to the internal
    // "storage" partition below.
    bool sd_ok = sdcard_mount();

    // Internal "storage" FAT partition for USB-MSC when there is no card.
    if (!storage_init("storage"))
        ESP_LOGW(TAG, "internal storage unavailable; USB-MSC drive disabled");

    usb_init();

    pins_init();

    xTaskCreate(&gdb_application_thread, "gdb_thread", 4096, NULL, 5, NULL);

#if defined(CONFIG_BOARD_CARDPUTER)
    ui_start();

    // Report the SD result on the Cardputer screen (no boot serial needed).
    {
        char line[64];
        if (sd_ok)
            snprintf(line, sizeof line, "sd: mounted %lu MB\n",
                     (unsigned long)(((uint64_t)sdcard_block_count() *
                                      sdcard_block_size()) >> 20));
        else
            snprintf(line, sizeof line, "sd: no mount (%s)\n",
                     esp_err_to_name(sdcard_last_err()));
        ui_capture_write(line);
    }
#endif

    led_set_green(0);

    ESP_LOGI(TAG, "end");
}
