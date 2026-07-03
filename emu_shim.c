/*
 * emu_shim.c - Bridge between Black Magic Probe's ADIv5 layer (adiv5.c/adiv5.h)
 * and the emulated STM32F103 target model (emu_target.c). Implements the four
 * DP primitives and a scan entry callable from a monitor command.
 *
 * Architecture:
 * - adiv5_dp_init() installs firmware_ap_read/write and mem_read/write callbacks.
 * - Those callbacks use dp->dp_read, dp->low_access, dp->error, dp->abort.
 * - We implement those four to dispatch into the emulated model.
 * - Routing: APnDP bit (0x100) in addr distinguishes DP vs AP accesses.
 * - AP reads are posted (return previous value, post current one) to match
 *   BMP's mem_read_bytes protocol (primer read + loop + RDBUFF final).
 */

#include "general.h"
#include "adiv5.h"
#include "emu_target.h"

/* Posted-read state: last AP read result to return on next read access. */
static uint32_t g_posted = 0;

/* Current AP selection (apsel bits 31:24 of SELECT register). */
static uint8_t g_apsel = 0;

/*
 * emu_shim_dp_read(dp, addr) - Handle DP-only reads (addresses without APnDP).
 * Called by firmware_ap_read during AP selection writes and adiv5_dp_read for
 * DPIDR, CTRL/STAT, RDBUFF, etc. Returns the DP register value directly.
 */
uint32_t emu_shim_dp_read(adiv5_debug_port_s *dp, uint16_t addr)
{
	(void)dp; /* Not used; model is global state. */
	return emu_dp_read(addr);
}

/*
 * emu_shim_low_access(dp, rnw, addr, value) - Handle both DP and AP transactions.
 * APnDP bit (0x100) in addr selects DP vs AP. DP accesses are immediate; AP
 * accesses to DRW are posted (return previous, post current). This matches BMP's
 * advi5_mem_read_bytes protocol.
 *
 * For AP reads (addr & 0x100 && rnw == READ):
 * - advi5_mem_read_bytes does: primer read, then loop reads, then RDBUFF final.
 * - We return g_posted (the previous read value) and store current in g_posted.
 *
 * For AP writes (addr & 0x100 && rnw == WRITE):
 * - advi5_mem_write_bytes writes to DRW, possibly re-selecting if addr crosses
 *   a 1KB boundary. SELECT writes are routed as DP operations here.
 */
uint32_t emu_shim_low_access(adiv5_debug_port_s *dp, uint8_t rnw, uint16_t addr, uint32_t value)
{
	(void)dp;

	if (addr & ADIV5_APnDP) {
		/* AP access: extract bank bits for routing. For AHB-AP0, all regions
		 * (CSW/TAR/DRW/BASE/IDR) use the same emu_ap_read/write interface.
		 * BMP's firmware_ap_read first writes SELECT to bank the AP into view,
		 * so we don't need to implement per-bank register offsets. */
		if (rnw == ADIV5_LOW_READ) {
			/* AP read (posted): return previous, post current.
			 * DRW (0x0c) returns data with auto-increment side effect;
			 * IDR/BASE/CSW (0xfc/0xf8/0x00) are configuration. */
			uint32_t ret = g_posted;
			g_posted = emu_ap_read(g_apsel, addr);
			return ret;
		} else {
			/* AP write: TAR, DRW, CSW, etc. */
			emu_ap_write(g_apsel, addr, value);
			return 0; /* Write returns are ignored. */
		}
	} else {
		/* DP access: immediate read/write. SELECT writes update g_apsel.
		 * firmware_ap_read and firmware_ap_write use SELECT to bank registers,
		 * so we extract apsel here for the posted-read model to work. */
		if (rnw == ADIV5_LOW_WRITE && addr == ADIV5_DP_SELECT) {
			g_apsel = value >> 24;
			emu_dp_low_access(ADIV5_LOW_WRITE, addr, value);
			return 0;
		} else if (rnw == ADIV5_LOW_READ) {
			return emu_dp_read(addr);
		} else {
			/* DP write: ABORT, CTRL/STAT, etc. */
			emu_dp_low_access(ADIV5_LOW_WRITE, addr, value);
			return 0;
		}
	}
}

/*
 * emu_shim_error(dp, protocol_recovery) - Acknowledge/clear errors on the DP.
 * The model never sets dp->fault, so this just delegates to the model's
 * error handler for consistency. protocol_recovery is ignored; the model
 * doesn't model line-level recovery protocols (SWD only).
 */
uint32_t emu_shim_error(adiv5_debug_port_s *dp, bool protocol_recovery)
{
	(void)dp;
	(void)protocol_recovery;
	return emu_dp_error();
}

/*
 * emu_shim_abort(dp, abort) - Assert the DAP ABORT register to recover from
 * error states. The model supports unlock/lock; abort bits are delegated.
 */
void emu_shim_abort(adiv5_debug_port_s *dp, uint32_t abort)
{
	(void)dp;
	emu_dp_abort(abort);
}

/*
 * emu_scan() - Stand up an emulated ADIv5 debug port and run normal enumeration.
 * Called from a monitor command (e.g., "monitor emulate"). Creates a DP struct,
 * installs the four shim callbacks, initializes the target model, then runs
 * adiv5_dp_init which probes APs and dispatches cortexm_probe, stm32f1_probe, etc.
 * On success returns true and the fake STM32F103 is in the target list.
 * On failure returns false and the DP is freed.
 *
 * This mirrors the structure of adiv5_swd_scan (command.c line 250), except
 * the "scan" is internal: no platform SWD layer is needed.
 */
bool emu_scan(void)
{
	/* Allocate the debug port struct. adiv5_dp_init will handle refcounting. */
	adiv5_debug_port_s *dp = malloc(sizeof(*dp));
	if (!dp)
		return false;

	memset(dp, 0, sizeof(*dp));

	/* Install the shim callbacks. adiv5_dp_init will overwrite ap_read/write
	 * and mem_read/write with firmware_ versions, which call these four. */
	dp->dp_read = emu_shim_dp_read;
	dp->low_access = emu_shim_low_access;
	dp->error = emu_shim_error;
	dp->abort = emu_shim_abort;

	/* Initialize posted-read and AP selection state. */
	g_posted = 0;
	g_apsel = 0;

	/* Bring up the emulated target model. This initializes the fake STM32F103
	 * SCS, debug registers, FPEC, memory, and ROM table. */
	emu_target_init();

	/* Run the standard ADIv5 DP initialization. This reads DPIDR, performs
	 * power-up sequence on CTRL/STAT, scans for APs (only AP0 responds),
	 * then probes the ROM table at ap->base. ROM walk finds SCS (part 0x000,
	 * class 0xe), dispatches cortexm_probe (recognizes ARM/0x4c3 Cortex-M3 ROM),
	 * which then calls stm32f1_probe (reads DBGMCU, finds dev 0x410 = medium
	 * density). The fake target is added to target_list if successful.
	 *
	 * If adiv5_dp_init fails, it frees the dp before returning.
	 */
	adiv5_dp_init(dp);

	return true;
}
