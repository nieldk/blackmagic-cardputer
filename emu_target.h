/*
 * emu_target.h - in-firmware emulated ADIv5 SW-DP + Cortex-M debug target
 *
 * This is a target-side model, not a CPU. It answers the ADIv5 DP/AP register
 * transactions and memory accesses that Black Magic Probe issues while it scans
 * and attaches, so `monitor swdp_scan` / `attach` / memory reads / flashing all
 * work against a fake STM32F103 with no physical target wired to the SWD pins.
 *
 * It does NOT execute target instructions. The emulated core is always halted;
 * run/step/breakpoints are out of scope for this build.
 *
 * Wiring: a thin shim provides an adiv5_debug_port_s whose accessor callbacks
 * forward into the emu_* functions below. cortexm.c / stm32f1.c then drive it
 * unmodified. See INTEGRATION notes at the bottom.
 */
#ifndef EMU_TARGET_H
#define EMU_TARGET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Access width for emu_mem_write, matches the AP CSW size field semantics. */
typedef enum emu_access {
	EMU_ACCESS_BYTE = 0, /* 8-bit  */
	EMU_ACCESS_HALF = 1, /* 16-bit */
	EMU_ACCESS_WORD = 2, /* 32-bit */
} emu_access_e;

/* Reset the whole model to its power-on state (core halted, flash erased). */
void emu_target_init(void);

/* --- DP transaction layer (SWD debug port) ------------------------------- *
 * addr is the ADIv5 register address (A[3:2] with the DP bank folded in by
 * the caller's SELECT handling, i.e. the value BMP passes to dp_read). */
uint32_t emu_dp_read(uint16_t addr);
uint32_t emu_dp_low_access(uint8_t rnw, uint16_t addr, uint32_t value);
void emu_dp_abort(uint32_t abort);
uint32_t emu_dp_error(void); /* returns and clears sticky CTRL/STAT errors */

/* --- AP transaction layer (MEM-AP) --------------------------------------- *
 * The current AP is selected via the DP SELECT register (APSEL). Only AP #0
 * (an AHB-AP) exists in this model; other APSEL values read as absent. */
uint32_t emu_ap_read(uint8_t apsel, uint16_t addr);
void emu_ap_write(uint8_t apsel, uint16_t addr, uint32_t value);

/* --- bulk memory access (used by BMP's ap->mem_read / mem_write) ---------- *
 * Routes through the same address map as AP TAR/DRW accesses: SRAM, flash,
 * the Cortex-M SCS/debug block, DBGMCU and the FPEC flash controller. */
void emu_mem_read(uint32_t addr, void *dest, size_t len);
void emu_mem_write(uint32_t addr, const void *src, size_t len, emu_access_e access);

/* True once emu_target_init has run and a scan would find the fake DP. Lets
 * the platform toggle emulation on/off (e.g. from a `monitor` command). */
bool emu_target_enabled(void);
void emu_target_set_enabled(bool on);

#endif /* EMU_TARGET_H */
