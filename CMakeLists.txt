# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# EXCLUDE_COMPONENTS is a GNU Make-build-system-only mechanism and is
# not read by the CMake build system at all - that's why it silently
# did nothing on three separate attempts. The actual documented CMake
# mechanism is the inverse: COMPONENTS is a whitelist, and anything a
# whitelisted component REQUIRES/PRIV_REQUIRES gets auto-expanded in.
# Since nothing this firmware actually depends on touches bt/esp_wifi/
# mbedtls/lwip, whitelisting just main should exclude all of it as a
# side effect.
set(COMPONENTS main)

set(COMMAND_SRC "${CMAKE_CURRENT_SOURCE_DIR}/command.c")
set(COMMAND_DST "${CMAKE_CURRENT_SOURCE_DIR}/components/blackmagic/blackmagic-fw/src/")
if(EXISTS "${COMMAND_SRC}")
    file(COPY "${COMMAND_SRC}" DESTINATION "${COMMAND_DST}")
    message(STATUS "Copied patched command.c into blackmagic-fw")
else()
    message(WARNING "command.c not found at repo root — submodule file will be used unpatched")
endif()

# Emulated-target sources. Unlike the two files above these are NEW files, not
# patches, but the same reasoning applies: keep them at the repo root and copy
# them into the submodule's src/target/ so a submodule checkout/reset never
# loses them. The .h files must land next to the .c files because src/target is
# an include dir (so command.c can #include "emu_shim.h" and emu_shim.c can
# find emu_target.h). This copy runs at configure time — if you edit any of
# these at the repo root, run `idf.py reconfigure` (or delete build/) so the
# fresh copy is picked up.
set(EMU_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(EMU_DST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/components/blackmagic/blackmagic-fw/src/target/")
foreach(emu_file emu_target.c emu_target.h emu_shim.c emu_shim.h)
    if(EXISTS "${EMU_SRC_DIR}/${emu_file}")
        file(COPY "${EMU_SRC_DIR}/${emu_file}" DESTINATION "${EMU_DST_DIR}")
        message(STATUS "Copied ${emu_file} into blackmagic-fw/src/target")
    else()
        message(WARNING "${emu_file} not found at repo root — emulator will not build")
    endif()
endforeach()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# portYIELD_FROM_ISR() on this IDF 5.1.4/xtensa-port pairing calls
# traceISR_EXIT_TO_SCHEDULER() unconditionally, expecting it to default
# to a no-op when trace facility tracing isn't enabled. Defining it as
# the no-op it's documented to default to.
add_compile_definitions("traceISR_EXIT_TO_SCHEDULER()=")

project(espbmp)
