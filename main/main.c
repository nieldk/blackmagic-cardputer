#include <stdint.h>
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
    // disable interrupt
    io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
    // set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    // bit mask of the pins that you want to set
    io_conf.pin_bit_mask = (
        ((uint64_t)1 << SWCLK_PIN)
        | ((uint64_t)1 << SWDIO_PIN)
#if TDO_PIN != -1
        | ((uint64_t)1 << TDO_PIN)
#endif
#if NRST_PIN != -1
        | ((uint64_t)1 << NRST_PIN)
#endif
// #if LOG_TX_PIN != -1
//         | ((uint64_t)1 << LOG_TX_PIN)
// #endif
    );
    // disable pull-down mode
    io_conf.pull_down_en = 0;
    // disable pull-up mode
    io_conf.pull_up_en = 0;
    // configure GPIO with the given settings
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

    gdb_glue_init();

    display_init();

    led_init();
    led_set_green(255);

    usb_init();

    pins_init();

    xTaskCreate(&gdb_application_thread, "gdb_thread", 4096, NULL, 5, NULL);

#if defined(CONFIG_BOARD_CARDPUTER)
    ui_start();
#endif

    led_set_green(0);

    ESP_LOGI(TAG, "end");
}
