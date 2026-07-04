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

# Automatically copy patched submodule files from the repo root before build.
# gdb_packet.c and command.c live in the blackmagic-fw submodule which we
# don't own — patches live at the repo root and are applied here automatically.
set(GDB_PACKET_SRC "${CMAKE_CURRENT_SOURCE_DIR}/gdb_packet.c")
set(GDB_PACKET_DST "${CMAKE_CURRENT_SOURCE_DIR}/components/blackmagic/blackmagic-fw/src/")
if(EXISTS "${GDB_PACKET_SRC}")
    file(COPY "${GDB_PACKET_SRC}" DESTINATION "${GDB_PACKET_DST}")
    message(STATUS "Copied patched gdb_packet.c into blackmagic-fw")
else()
    message(WARNING "gdb_packet.c not found at repo root — submodule file will be used unpatched")
endif()

set(COMMAND_SRC "${CMAKE_CURRENT_SOURCE_DIR}/command.c")
set(COMMAND_DST "${CMAKE_CURRENT_SOURCE_DIR}/components/blackmagic/blackmagic-fw/src/")
if(EXISTS "${COMMAND_SRC}")
    file(COPY "${COMMAND_SRC}" DESTINATION "${COMMAND_DST}")
    message(STATUS "Copied patched command.c into blackmagic-fw")
else()
    message(WARNING "command.c not found at repo root — submodule file will be used unpatched")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# portYIELD_FROM_ISR() on this IDF 5.1.4/xtensa-port pairing calls
# traceISR_EXIT_TO_SCHEDULER() unconditionally, expecting it to default
# to a no-op when trace facility tracing isn't enabled. Defining it as
# the no-op it's documented to default to.
add_compile_definitions("traceISR_EXIT_TO_SCHEDULER()=")

project(espbmp)