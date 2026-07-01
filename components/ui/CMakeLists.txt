idf_component_register(
    SRCS "ui.c"
    INCLUDE_DIRS "."
    REQUIRES blackmagic hagl hagl_hal keyboard tinyusb driver freertos
)
