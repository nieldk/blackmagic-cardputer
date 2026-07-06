/*
 * emu_target.h - in-firmware emulated STM32F103 (medium density) target model.
 *
 * Provides a synthetic ADIv5 memory + register space so Black Magic Probe's
 * own adiv5.c / cortexm.c / stm32f1.c code can enumerate, attach to, program
 * and verify a target with no SWD pins driven. All constants below were
 * derived directly from the BMP source this integrates with (adiv5.c,
 * cortexm.c, stm32f1.c), so the enumeration and flash paths match byte-for-byte.
 *
 * The model is a byte-addressable memory function. The shim (emu_shim.c) turns
 * adiv5 DP/AP transactions into sized load/store calls on this model.
 */
#ifndef EMU_TARGET_H
#define EMU_TARGET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---- Memory map (STM32F103 medium density, device id 0x410) ------------- */
#define EMU_FLASH_BASE 0x08000000U
#define EMU_FLASH_SIZE 0x00020000U /* 128 KiB */
#define EMU_FLASH_PAGE 0x00000400U /* 1 KiB erase/program page (writesize)   */
#define EMU_SRAM_BASE  0x20000000U
#define EMU_SRAM_SIZE  0x00005000U /* 20 KiB  */

/* ---- Debug infrastructure addresses ------------------------------------- */
#define EMU_SCS_BASE       0xE000E000U /* CORTEXM_SCS_BASE  */
#define EMU_ROM_BASE       0xE00FF000U /* Cortex-M3 ROM table */
#define EMU_DBGMCU_IDCODE  0xE0042000U /* stm32f1 DBGMCU_IDCODE */
#define EMU_FPEC_BASE      0x40022000U /* Flash program/erase controller */

/* ---- Values the enumeration path checks against ------------------------- *
 * DPIDR:  DPv1, JEP106 designer ARM (0x43b), partno 0xba, bit0 RAO.
 *         Real STM32F103 SW-DP value.                                        */
#define EMU_DPIDR 0x1BA01477U

/* AHB-AP IDR: CLASS=8 (MEM-AP), TYPE=1 (AHB), DESIGNER=ARM, REV=1.
 * REV<=1 so stm32f1_probe does NOT tag it as a clone. */
#define EMU_AP_IDR  0x14770011U
#define EMU_AP_BASE 0xE00FF003U /* ROM table @ 0xE00FF000, present + ADIv5 fmt */
#define EMU_AP_CFG  0x00000000U

/* DBGMCU_IDCODE: low 12 bits = 0x410 (medium density). */
#define EMU_IDCODE 0x20036410U

/* Cortex-M3 CPUID (r2p1). partno field (& 0xfff0) == CORTEX_M3 (0xc230). */
#define EMU_CPUID 0x412FC231U

/* ---- Model entry points ------------------------------------------------- *
 * size is in bytes: 1, 2 or 4. Values are right-justified (low bytes); lane
 * placement is the caller's (shim's) responsibility, matching ADIv5.         */
uint32_t emu_target_load(uint32_t addr, uint32_t size);
void emu_target_store(uint32_t addr, uint32_t value, uint32_t size);

/* Word convenience wrappers (used by the ID/register paths and tests). */
uint32_t emu_target_read_word(uint32_t addr);
void emu_target_write_word(uint32_t addr, uint32_t value);

void emu_target_reset(void); /* free flash pages, clear SRAM, re-seed state */

#endif /* EMU_TARGET_H */