/*
 * test_emu.c - host-side validation of emu_target.c.
 *
 * Reproduces the exact register-decode math from BMP's adiv5.c / cortexm.c /
 * stm32f1.c and asserts the model answers each read the way the enumeration
 * path requires. Also reimplements the posted-read pipeline used by the shim
 * to confirm DRW/RDBUFF sequencing unpacks memory correctly.
 *
 *   gcc -Wall -Wextra test_emu.c emu_target.c -o test_emu && ./test_emu
 */
#include "emu_target.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...)                       \
	do {                                       \
		if (cond) {                            \
			printf("  ok   : " __VA_ARGS__);   \
			printf("\n");                      \
		} else {                               \
			printf("  FAIL : " __VA_ARGS__);   \
			printf("\n");                      \
			++fails;                           \
		}                                      \
	} while (0)

/* ---- BMP decode helpers, copied verbatim in spirit from adiv5.c --------- */

/* adiv5_ap_read_id: 16-byte read, keep bytes [0],[4],[8],[12]. */
static uint32_t ap_read_id(uint32_t addr)
{
	uint32_t res = 0;
	for (uint32_t i = 0; i < 4U; ++i)
		res |= (emu_target_read_word(addr + i * 4U) & 0xffU) << (i * 8U);
	return res;
}

/* adiv5_ap_read_pidr: PIDR4 block << 32 | PIDR0 block. */
static uint64_t ap_read_pidr(uint32_t addr)
{
	uint64_t pidr = ap_read_id(addr + 0xFD0U);          /* PIDR4..7 */
	pidr = pidr << 32U | ap_read_id(addr + 0xFE0U);     /* PIDR0..3 */
	return pidr;
}

/* JEP106 designer decode from PIDR (adiv5.c component_probe). */
static uint16_t pidr_designer(uint64_t pidr)
{
	const uint64_t JEP106_USED = (uint64_t)1 << 19U;
	const uint64_t CONT_MASK = (uint64_t)0xf << 32U;
	const uint64_t CODE_MASK = (uint64_t)0x7f << 12U;
	if (pidr & JEP106_USED)
		return (uint16_t)((pidr & CONT_MASK) >> (32U - 8U) | (pidr & CODE_MASK) >> 12U);
	return (uint16_t)((pidr & CODE_MASK) >> 12U | 0x80U);
}

/* DPIDR designer decode from adiv5_dp_init. */
static uint16_t dpidr_designer(uint32_t dpidr)
{
	uint16_t designer = (dpidr >> 1U) & 0x7ffU;
	return (uint16_t)((designer & (0xfU << 7U)) << 1U | (designer & 0x7fU));
}

int main(void)
{
	emu_target_reset();
	printf("== emulated STM32F103 model validation ==\n");

	/* --- DP identity --- */
	uint32_t dpidr = EMU_DPIDR;
	CHECK((dpidr & 1U) == 1U, "DPIDR bit0 RAO");
	CHECK(((dpidr >> 12U) & 0xfU) == 1U, "DPIDR version = DPv1");
	CHECK(dpidr_designer(dpidr) == 0x43bU, "DPIDR designer decodes to ARM (0x43b)");

	/* --- AHB-AP identity --- */
	uint32_t idr = EMU_AP_IDR;
	CHECK(((idr >> 13U) & 0xfU) == 8U, "AP IDR class = 8 (MEM-AP)");
	CHECK((idr & 0xfU) == 1U, "AP IDR type = 1 (AHB)");
	CHECK((idr >> 28U) <= 1U, "AP IDR revision <= 1 (not tagged clone)");
	CHECK((EMU_AP_BASE & 0xfffff000U) == EMU_ROM_BASE, "AP BASE -> ROM table 0xE00FF000");
	CHECK((EMU_AP_BASE & 1U) == 1U, "AP BASE present bit set");

	/* --- ROM table --- */
	uint32_t rom_cidr = ap_read_id(EMU_ROM_BASE + 0xFF0U);
	CHECK((rom_cidr & ~0x0000f000U) == 0xb105000dU, "ROM CIDR preamble ok");
	CHECK(((rom_cidr & 0x0000f000U) >> 12U) == 0x1U, "ROM CIDR class = 1 (ROM table)");
	uint64_t rom_pidr = ap_read_pidr(EMU_ROM_BASE);
	CHECK((rom_pidr & 0xfffU) == 0x4c3U, "ROM PIDR part = 0x4c3 (Cortex-M3 ROM)");
	CHECK(pidr_designer(rom_pidr) == 0x020U, "ROM PIDR designer = STMicro (0x020)");

	/* ROM entry 0 -> SCS at 0xE000E000 */
	uint32_t entry0 = emu_target_read_word(EMU_ROM_BASE + 0x000U);
	uint32_t scs_addr = EMU_ROM_BASE + (entry0 & 0xfffff000U);
	CHECK((entry0 & 1U) == 1U, "ROM entry0 present");
	CHECK(scs_addr == EMU_SCS_BASE, "ROM entry0 -> SCS @ 0xE000E000 (got 0x%08x)", scs_addr);
	CHECK(emu_target_read_word(EMU_ROM_BASE + 0x004U) == 0U, "ROM entry1 = terminator");

	/* --- SCS component --- */
	uint32_t scs_cidr = ap_read_id(scs_addr + 0xFF0U);
	CHECK((scs_cidr & ~0x0000f000U) == 0xb105000dU, "SCS CIDR preamble ok");
	CHECK(((scs_cidr & 0x0000f000U) >> 12U) == 0xeU, "SCS CIDR class = 0xe (gipc)");
	uint64_t scs_pidr = ap_read_pidr(scs_addr);
	CHECK((scs_pidr & 0xfffU) == 0x000U, "SCS PIDR part = 0x000 (Cortex-M3 SCS)");
	CHECK(pidr_designer(scs_pidr) == 0x43bU, "SCS PIDR designer = ARM (0x43b)");

	/* --- cortexm_probe: CPUID --- */
	uint32_t cpuid = emu_target_read_word(scs_addr + 0xD00U);
	CHECK((cpuid & 0x0000fff0U) == 0xc230U, "CPUID partno = Cortex-M3 (0xc230)");

	/* --- CPACR readback => non-FP (v7m) --- */
	emu_target_write_word(scs_addr + 0xD88U, 0x00f00000U);
	CHECK(emu_target_read_word(scs_addr + 0xD88U) != 0x00f00000U, "CPACR readback != written => no FPU");

	/* --- DHCSR: halted, no reset-sticky (halt loops terminate) --- */
	uint32_t dhcsr = emu_target_read_word(scs_addr + 0xDF0U);
	CHECK(!(dhcsr & (1U << 25U)), "DHCSR S_RESET_ST clear");
	CHECK(dhcsr & (1U << 17U), "DHCSR S_HALT set");

	/* --- stm32f1_probe: DBGMCU_IDCODE --- */
	uint32_t idcode = emu_target_read_word(EMU_DBGMCU_IDCODE) & 0xfffU;
	CHECK(idcode == 0x410U, "DBGMCU_IDCODE low12 = 0x410 (medium density)");

	/* --- posted-read pipeline (shim algorithm) over a 16-byte flash read --- */
	{
		uint32_t src = EMU_FLASH_BASE;
		uint32_t tar = src, posted = 0xdeadbeefU;
		uint8_t out[16];
		uint8_t *d = out;
		/* prime */
		(void)posted;
		posted = emu_target_read_word(tar);
		tar += 4;
		/* three DRW reads returning previous posted */
		for (int i = 0; i < 3; ++i) {
			uint32_t v = posted;
			posted = emu_target_read_word(tar);
			tar += 4;
			memcpy(d, &v, 4);
			d += 4;
		}
		/* RDBUFF returns last posted */
		memcpy(d, &posted, 4);
		uint32_t w0, w3;
		memcpy(&w0, out + 0, 4);
		memcpy(&w3, out + 12, 4);
		CHECK(w0 == 0x20005000U, "posted read word0 = initial SP (0x20005000)");
		CHECK(w3 == emu_target_read_word(EMU_FLASH_BASE + 12U), "posted read word3 matches direct read");
	}

	/* --- FPEC unlock + program roundtrip --- */
	emu_target_write_word(0x40022004U, 0x45670123U); /* KEYR key1 */
	emu_target_write_word(0x40022004U, 0xCDEF89ABU); /* KEYR key2 */
	emu_target_write_word(EMU_FLASH_BASE + 0x40U, 0xa5a5a5a5U);
	CHECK(emu_target_read_word(EMU_FLASH_BASE + 0x40U) == 0xa5a5a5a5U, "FPEC-unlocked flash program roundtrip");

	printf("\n%s (%d failure%s)\n", fails ? "FAILURES" : "ALL PASS", fails, fails == 1 ? "" : "s");
	return fails ? 1 : 0;
}
