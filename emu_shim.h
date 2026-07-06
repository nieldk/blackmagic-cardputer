/*
 * emu_shim.h - bridges Black Magic Probe's ADIv5 layer to the emulated
 * STM32F103 model in emu_target.c. Provides the four DP primitives BMP's
 * adiv5_dp_init() needs, plus a scan entry that stands up a synthetic SWD-DP
 * with no pins driven.
 */
#ifndef EMU_SHIM_H
#define EMU_SHIM_H

#include "general.h"
#include "adiv5.h"

/*
 * Allocate a synthetic debug port, install the emulated DP primitives, and run
 * the standard ADIv5 enumeration (adiv5_dp_init) against it. On success the
 * fake STM32F103 is probed and registered in the target list exactly as a real
 * scan would leave it. Returns true if the DP was created.
 *
 * Call this from a monitor command (see INTEGRATION.md).
 */
bool emu_scan(void);

/* DP primitives, installed by emu_scan. Exposed for testing/alt wiring. */
uint32_t emu_shim_dp_read(adiv5_debug_port_s *dp, uint16_t addr);
uint32_t emu_shim_low_access(adiv5_debug_port_s *dp, uint8_t rnw, uint16_t addr, uint32_t value);
uint32_t emu_shim_error(adiv5_debug_port_s *dp, bool protocol_recovery);
void emu_shim_abort(adiv5_debug_port_s *dp, uint32_t abort);

#endif /* EMU_SHIM_H */
