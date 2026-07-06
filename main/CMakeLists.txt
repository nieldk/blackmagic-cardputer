set(SOURCES
    "main.c"
    "delay.c"
    "usb.c"
    "usb-uart.c"
    "led.c"
    "soft-uart-log.c"
    "display.c"
    "sdcard.c"
)

set (INCLUDES ".")

idf_component_register(SRCS ${SOURCES}
                    INCLUDE_DIRS ${INCLUDES}
                    PRIV_REQUIRES blackmagic tinyusb simple-uart soft-uart hagl hagl_hal ui driver fatfs sdmmc nvs_flash)
