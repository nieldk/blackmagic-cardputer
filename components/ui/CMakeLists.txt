idf_component_register(
    SRCS "ui.c" "ui_debug.c" "bmp_standalone_load.c" "target_lock.c" "serial_flash.c"
    INCLUDE_DIRS "."
    REQUIRES blackmagic hagl hagl_hal keyboard tinyusb driver freertos nvs_flash 
             espressif__esp-serial-flasher
)
