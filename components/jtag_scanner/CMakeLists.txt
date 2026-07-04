idf_component_register(
    SRCS "jtag_scanner.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp_timer
)