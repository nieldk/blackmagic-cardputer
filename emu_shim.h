/*
 * emu_shim.h - glue between Black Magic Probe's ADIv5 layer and the emulated
 * target model (emu_target.c). Provides the four DP primitives BMP needs and a
 * scan entry that stands up a synthetic debug port with no SWD pins involved.
 */
#ifndef EMU_SHIM_H
#define EMU_SHIM_H

#include "general.h"
#include "adiv5.h"

/*
 * Stand up the emulated debug port and run the normal ADIv5 enumeration against
 * it (adiv5_dp_init). On return the fake STM32F103 has been probed and, if
 * successful, registered in the target list exactly as a real scan would leave
 * it. Returns true if the DP was created (matching adiv5_swd_scan semantics).
 *
 * Call this from a monitor command; see the command.c patch in INTEGRATION.md.
 */
bool emu_scan(void);

/* DP primitive callbacks installed on the synthetic adiv5_debug_port_s. Exposed
 * for testing / alternative wiring; emu_scan installs them for you. */
uint32_t emu_shim_dp_read(adiv5_debug_port_s *dp, uint16_t addr);
uint32_t emu_shim_low_access(adiv5_debug_port_s *dp, uint8_t rnw, uint16_t addr, uint32_t value);
uint32_t emu_shim_error(adiv5_debug_port_s *dp, bool protocol_recovery);
void emu_shim_abort(adiv5_debug_port_s *dp, uint32_t abort);

#endif /* EMU_SHIM_H */
