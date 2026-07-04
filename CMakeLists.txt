# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Automatically copy the patched gdb_packet.c into the blackmagic-fw submodule
# before the build runs. The submodule itself is a separate git repo we don't
# own, so the patch lives in the root of this repo instead and gets applied here.
# This means a fresh clone + submodule init will always get the right file
# without any manual step.
set(GDB_PACKET_SRC "${CMAKE_CURRENT_SOURCE_DIR}/gdb_packet.c")
set(GDB_PACKET_DST "${CMAKE_CURRENT_SOURCE_DIR}/components/blackmagic/blackmagic-fw/src/gdb_packet.c")
if(EXISTS "${GDB_PACKET_SRC}")
    file(COPY "${GDB_PACKET_SRC}" DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}/components/blackmagic/blackmagic-fw/src/")
    message(STATUS "Copied patched gdb_packet.c into blackmagic-fw")
else()
    message(WARNING "gdb_packet.c not found at repo root — submodule file will be used unpatched")
endif()

# This firmware never uses networking, Bluetooth, or TLS - but ESP-IDF
# builds and statically links every discovered component by default.
# Several of these (lwip's pbuf/TCP pools, esp_wifi's RX/TX buffer
# arrays, mbedTLS's default tables) reserve large fixed-size global
# arrays in .bss/.data regardless of whether their init functions are
# ever called, which is what was blowing the DRAM segment by ~775KB on
# a no-PSRAM ESP32-S3FN8 (Cardputer). Excluding them at the component-
# discovery level removes their .o files from the link entirely.
set(EXCLUDE_COMPONENTS
    bt
    esp_wifi
    esp_coex
    wifi_provisioning
    openthread
    mbedtls
    lwip
    esp_eth
    esp_netif
    esp-tls
    esp_http_client
    esp_http_server
    esp_https_ota
    esp_https_server
    mqtt
    tcp_transport
    protocomm
    protobuf-c
    json
    esp_local_ctrl
    espcoredump
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# portYIELD_FROM_ISR() on this IDF 5.1.4/xtensa-port pairing calls
# traceISR_EXIT_TO_SCHEDULER() unconditionally, expecting it to default
# to a no-op when trace facility tracing isn't enabled - that default
# isn't being picked up for tinyusb's compilation units, causing an
# undefined reference at link time. Defining it as the no-op it's
# documented to default to. Must come before project() since that's
# what triggers component traversal.
add_compile_definitions("traceISR_EXIT_TO_SCHEDULER()=")

project(espbmp)
