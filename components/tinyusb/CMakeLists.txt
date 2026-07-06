idf_component_register(REQUIRES esp_rom app_update spi_flash freertos soc driver main fatfs wear_levelling esp_partition)

idf_component_get_property(FREERTOS_ORIG_INCLUDE_PATH freertos ORIG_INCLUDE_PATH)

idf_build_get_property(idf_target IDF_TARGET)

# portYIELD_FROM_ISR() on IDF 5.1.4's xtensa port calls
# traceISR_EXIT_TO_SCHEDULER() unconditionally, expecting it to default
# to a no-op when trace facility tracing isn't enabled - that default
# isn't reaching these directly-into-the-executable-compiled sources,
# causing an undefined reference at link time. Define it as the no-op
# it's documented to default to, via the same INTERFACE mechanism the
# CFG_TUSB_MCU defines below already use.
target_compile_options(${COMPONENT_TARGET} INTERFACE
    "-DtraceISR_EXIT_TO_SCHEDULER()="
)

if(${idf_target} STREQUAL "esp32s2")
    target_compile_options(${COMPONENT_TARGET} INTERFACE
        "-DCFG_TUSB_MCU=OPT_MCU_ESP32S2"
    )
endif()

if(${idf_target} STREQUAL "esp32s3")
    target_compile_options(${COMPONENT_TARGET} INTERFACE
        "-DCFG_TUSB_MCU=OPT_MCU_ESP32S3"
    )
endif()

target_include_directories(${COMPONENT_TARGET} INTERFACE
    "${FREERTOS_ORIG_INCLUDE_PATH}"
    "${COMPONENT_DIR}/config/"
    "${COMPONENT_DIR}/drivers/"
    "${COMPONENT_DIR}/drivers/dual-cdc/"
    "${COMPONENT_DIR}/drivers/cdc-msc/"
    "${COMPONENT_DIR}/tinyusb/hw/bsp/"
    "${COMPONENT_DIR}/tinyusb/src/"
    "${COMPONENT_DIR}/tinyusb/src/device"
    "${COMPONENT_DIR}/tinyusb/src/class"
)

target_sources(${COMPONENT_TARGET} INTERFACE
    "${COMPONENT_DIR}/tinyusb/src/tusb.c"
    "${COMPONENT_DIR}/tinyusb/src/common/tusb_fifo.c"
    "${COMPONENT_DIR}/tinyusb/src/device/usbd.c"
    "${COMPONENT_DIR}/tinyusb/src/device/usbd_control.c"
    "${COMPONENT_DIR}/tinyusb/src/class/audio/audio_device.c"
    "${COMPONENT_DIR}/tinyusb/src/class/bth/bth_device.c"
    "${COMPONENT_DIR}/tinyusb/src/class/cdc/cdc_device.c"
    "${COMPONENT_DIR}/tinyusb/src/class/cdc/cdc_host.c"
    "${COMPONENT_DIR}/tinyusb/src/class/cdc/cdc_rndis_host.c"
    "${COMPONENT_DIR}/tinyusb/src/class/dfu/dfu_rt_device.c"
    "${COMPONENT_DIR}/tinyusb/src/class/dfu/dfu_device.c"
    "${COMPONENT_DIR}/tinyusb/src/class/hid/hid_device.c"
    "${COMPONENT_DIR}/tinyusb/src/class/hid/hid_host.c"
    "${COMPONENT_DIR}/tinyusb/src/class/midi/midi_device.c"
    "${COMPONENT_DIR}/tinyusb/src/class/msc/msc_device.c"
    "${COMPONENT_DIR}/tinyusb/src/class/msc/msc_host.c"
    "${COMPONENT_DIR}/tinyusb/src/class/net/ncm_device.c"
    "${COMPONENT_DIR}/tinyusb/src/class/net/ecm_rndis_device.c"
    "${COMPONENT_DIR}/tinyusb/src/class/usbtmc/usbtmc_device.c"
    "${COMPONENT_DIR}/tinyusb/src/class/vendor/vendor_device.c"
    "${COMPONENT_DIR}/tinyusb/src/class/vendor/vendor_host.c"
    "${COMPONENT_DIR}/tinyusb/src/class/video/video_device.c"
    "${COMPONENT_DIR}/tinyusb/src/portable/espressif/esp32sx/dcd_esp32sx.c"
    "${COMPONENT_DIR}/tinyusb/src/host/hub.c"
    "${COMPONENT_DIR}/tinyusb/src/host/usbh.c"

    "${COMPONENT_DIR}/drivers/usb-glue.c"

    "${COMPONENT_DIR}/drivers/storage.c"
    "${COMPONENT_DIR}/drivers/msc_disk.c"

    "${COMPONENT_DIR}/drivers/dual-cdc/dual-cdc-descriptors.c"

    "${COMPONENT_DIR}/drivers/cdc-msc/cdc-msc-descriptors.c"

    "${COMPONENT_DIR}/drivers/dap-link/dap-link-descriptors.c"
)
