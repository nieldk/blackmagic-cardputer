set(BM_DIR "blackmagic-fw")
set(BM_TARGET_DIR ${BM_DIR}/src/target)
set(PLATFORM_DIR "esp32-platform")

set(BM_SOURCES
    ${BM_DIR}/src/platforms/common/swdptap.c
    ${BM_DIR}/src/platforms/common/jtagtap.c
    ${BM_DIR}/src/command.c
    ${BM_DIR}/src/crc32.c
    ${BM_DIR}/src/exception.c
    ${BM_DIR}/src/gdb_main.c
    ${BM_DIR}/src/gdb_packet.c
    ${BM_DIR}/src/hex_utils.c
    ${BM_DIR}/src/main.c
    ${BM_DIR}/src/maths_utils.c
    ${BM_DIR}/src/morse.c
    ${BM_DIR}/src/remote.c
    ${BM_DIR}/src/rtt.c
    ${BM_TARGET_DIR}/adiv5.c
    ${BM_TARGET_DIR}/adiv5_jtag.c
    ${BM_TARGET_DIR}/adiv5_swd.c
    ${BM_TARGET_DIR}/at32f43x.c
    ${BM_TARGET_DIR}/ch32f1.c
    ${BM_TARGET_DIR}/cortex.c
    ${BM_TARGET_DIR}/cortexar.c
    ${BM_TARGET_DIR}/cortexm.c
    ${BM_TARGET_DIR}/efm32.c
    ${BM_TARGET_DIR}/gdb_reg.c
    ${BM_TARGET_DIR}/hc32l110.c
    ${BM_TARGET_DIR}/imxrt.c
    ${BM_TARGET_DIR}/jtag_devs.c
    ${BM_TARGET_DIR}/jtag_scan.c
    # ${BM_TARGET_DIR}/jtagtap_generic.c
    ${BM_TARGET_DIR}/kinetis.c
    ${BM_TARGET_DIR}/lmi.c
    ${BM_TARGET_DIR}/lpc_common.c
    ${BM_TARGET_DIR}/lpc11xx.c
    ${BM_TARGET_DIR}/lpc15xx.c
    ${BM_TARGET_DIR}/lpc17xx.c
    ${BM_TARGET_DIR}/lpc40xx.c
    ${BM_TARGET_DIR}/lpc43xx.c
    ${BM_TARGET_DIR}/lpc55xx.c
    ${BM_TARGET_DIR}/lpc546xx.c
    ${BM_TARGET_DIR}/msp432e4.c
    ${BM_TARGET_DIR}/msp432p4.c
    ${BM_TARGET_DIR}/nrf51.c
    ${BM_TARGET_DIR}/nrf91.c
    ${BM_TARGET_DIR}/nxpke04.c
    ${BM_TARGET_DIR}/renesas_ra.c
    ${BM_TARGET_DIR}/renesas_rz.c
    ${BM_TARGET_DIR}/riscv_debug.c
    ${BM_TARGET_DIR}/riscv_jtag_dtm.c
    ${BM_TARGET_DIR}/riscv32.c
    ${BM_TARGET_DIR}/riscv64.c
    ${BM_TARGET_DIR}/rp.c
    ${BM_TARGET_DIR}/sam3x.c
    ${BM_TARGET_DIR}/sam4l.c
    ${BM_TARGET_DIR}/samd.c
    ${BM_TARGET_DIR}/samx5x.c
    ${BM_TARGET_DIR}/semihosting.c
    ${BM_TARGET_DIR}/sfdp.c
    ${BM_TARGET_DIR}/spi.c
    ${BM_TARGET_DIR}/stm32f1.c
    ${BM_TARGET_DIR}/stm32f4.c
    ${BM_TARGET_DIR}/stm32g0.c
    ${BM_TARGET_DIR}/stm32h5.c
    ${BM_TARGET_DIR}/stm32h7.c
    ${BM_TARGET_DIR}/stm32l0.c
    ${BM_TARGET_DIR}/stm32l4.c
    ${BM_TARGET_DIR}/stm32mp15.c
    # ${BM_TARGET_DIR}/swdptap_generic.c
    ${BM_TARGET_DIR}/target_flash.c
    ${BM_TARGET_DIR}/target_probe.c
    ${BM_TARGET_DIR}/target.c
    ${BM_TARGET_DIR}/zynq7000.c
)

set(BM_OVERRIDES
    ${PLATFORM_DIR}/platform.c
    ${PLATFORM_DIR}/gdb-glue.c
    ${PLATFORM_DIR}/usb_serial.c
)

set(BM_INCLUDE
    ${BM_DIR}/src/include
    ${BM_DIR}/src/platforms/common
    ${BM_DIR}/src/target
    ${BM_DIR}/deps/libopencm3/include
    ${PLATFORM_DIR}
)

set(BM_REQUIREMENTS
    driver
    freertos
    hal
    esp_timer
    tinyusb
)

# Get the current working branch
execute_process(
    COMMAND git describe --always --dirty --tags
    WORKING_DIRECTORY ${COMPONENT_DIR}/blackmagic-fw
    OUTPUT_VARIABLE BM_GIT_DESC
    OUTPUT_STRIP_TRAILING_WHITESPACE)

message(STATUS "BM version: ${BM_GIT_DESC}")

idf_component_register(SRCS ${BM_SOURCES} ${BM_OVERRIDES}
    INCLUDE_DIRS ${BM_INCLUDE}
    PRIV_REQUIRES ${BM_REQUIREMENTS})

target_compile_options(${COMPONENT_LIB} PRIVATE -DPC_HOSTED=0 -DFIRMWARE_VERSION="${BM_GIT_DESC}" -DENABLE_DEBUG=1 -DENABLE_RTT=1 -Wno-char-subscripts -Wno-attributes)

set_property(SOURCE ${BM_SOURCES} PROPERTY COMPILE_FLAGS -std=c11)
