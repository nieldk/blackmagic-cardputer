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

# NOTE: command.c, gdb_packet.c and the emu_target/emu_shim sources now live
# directly in components/blackmagic/blackmagic-fw/src (and .../src/target for
# the emu_* files), which is vendored in-tree. There is no longer a repo-root
# copy step - edit those files in place.

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# portYIELD_FROM_ISR() on this IDF 5.1.4/xtensa-port pairing calls
# traceISR_EXIT_TO_SCHEDULER() unconditionally, expecting it to default
# to a no-op when trace facility tracing isn't enabled. Defining it as
# the no-op it's documented to default to.
add_compile_definitions("traceISR_EXIT_TO_SCHEDULER()=")

project(espbmp)
