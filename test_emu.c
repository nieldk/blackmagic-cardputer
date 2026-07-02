/* test_emu.c - host harness that drives emu_target the way BMP's ADIv5 layer
 * does during swdp_scan / attach / memory access / flash. Build and run on the
 * host (no ESP-IDF needed) to validate the model before wiring it into BMP. */
#include "emu_target.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int fails = 0;
#define CHECK(cond, ...) do { \
	if (cond) { printf("  ok   : " __VA_ARGS__); printf("\n"); } \
	else { printf("  FAIL : " __VA_ARGS__); printf("\n"); fails++; } \
} while (0)

/* mimic BMP's cortex-m reg read via DCRSR/DCRDR */
static uint32_t cm_reg_read(unsigned idx)
{
	emu_mem_write(0xe000edf4u, &idx, 4, EMU_ACCESS_WORD); /* DCRSR = idx (read) */
	uint32_t v;
	emu_mem_read(0xe000edf8u, &v, 4);                     /* DCRDR              */
	return v;
}

int main(void)
{
	emu_target_init();

	/* 1. DP IDCODE (what a line-reset + read of DP reg 0 returns) */
	uint32_t dpidr = emu_dp_read(0x0);
	CHECK(dpidr == 0x1ba01477u, "DPIDR = 0x%08x", dpidr);

	/* 2. debug/system power-up handshake */
	emu_dp_low_access(0 /*W*/, 0x4 /*CTRL/STAT*/, (1u<<28)|(1u<<30));
	uint32_t cs = emu_dp_read(0x4);
	CHECK((cs & (1u<<29)) && (cs & (1u<<31)), "CTRL/STAT power-up ack = 0x%08x", cs);

	/* 3. AP IDR identifies an AHB-AP (MEM-AP) */
	uint32_t apidr = emu_ap_read(0, 0xfc);
	CHECK(apidr == 0x14770011u, "AP[0] IDR = 0x%08x", apidr);

	/* 4. AP BASE points at the ROM table */
	uint32_t base = emu_ap_read(0, 0xf8);
	CHECK((base & ~0xfffu) == 0xe00ff000u, "AP[0] BASE -> ROM @0x%08x", base & ~0xfffu);

	/* 5. ROM table is CoreSight class 1 and its first entry points at the SCS */
	uint32_t cid1;
	emu_mem_read(0xe00ff000u + 0xff4u, &cid1, 4);
	CHECK(((cid1 >> 4) & 0xf) == 0x1, "ROM CIDR1 class = 0x%x (ROM table)", (cid1 >> 4) & 0xf);
	uint32_t entry0;
	emu_mem_read(0xe00ff000u, &entry0, 4);
	uint32_t scs = 0xe00ff000u + (entry0 & 0xfffff000u);
	CHECK(scs == 0xe000e000u, "ROM entry[0] -> SCS @0x%08x", scs);

	/* 6. CPUID at SCS+0xD00 identifies a Cortex-M3 (part 0xC23) */
	uint32_t cpuid;
	emu_mem_read(0xe000ed00u, &cpuid, 4);
	CHECK(((cpuid >> 4) & 0xfff) == 0xc23, "CPUID part = 0x%03x (Cortex-M3)", (cpuid >> 4) & 0xfff);

	/* 7. DBGMCU_IDCODE fingerprints STM32F103 medium density (dev 0x410) */
	uint32_t idc;
	emu_mem_read(0xe0042000u, &idc, 4);
	CHECK((idc & 0xfff) == 0x410, "DBGMCU_IDCODE dev = 0x%03x", idc & 0xfff);

	/* 8. attach: halt the core, confirm S_HALT */
	uint32_t halt = 0xa05f0000u | (1u<<0) | (1u<<1); /* DBGKEY|C_DEBUGEN|C_HALT */
	emu_mem_write(0xe000edf0u, &halt, 4, EMU_ACCESS_WORD);
	uint32_t dhcsr;
	emu_mem_read(0xe000edf0u, &dhcsr, 4);
	CHECK(dhcsr & (1u<<17), "DHCSR S_HALT set (core halted) = 0x%08x", dhcsr);

	/* 9. core register file access via DCRSR/DCRDR: write r0 then read it back */
	uint32_t r0 = 0xdeadbeefu;
	emu_mem_write(0xe000edf8u, &r0, 4, EMU_ACCESS_WORD);      /* DCRDR = value  */
	uint32_t dcrsr_w = (1u<<16) | 0u;                        /* write r0       */
	emu_mem_write(0xe000edf4u, &dcrsr_w, 4, EMU_ACCESS_WORD);
	CHECK(cm_reg_read(0) == 0xdeadbeefu, "core reg r0 round-trips via DCRSR/DCRDR");

	/* 10. SRAM read/write through the AP memory path */
	uint32_t pat = 0xa5a51234u;
	emu_mem_write(0x20000000u, &pat, 4, EMU_ACCESS_WORD);
	uint32_t back = 0;
	emu_mem_read(0x20000000u, &back, 4);
	CHECK(back == pat, "SRAM @0x20000000 round-trip = 0x%08x", back);

	/* also exercise the AP TAR/DRW auto-increment path BMP uses for blocks */
	uint32_t tar = 0x20000010u;
	emu_ap_write(0, 0x04, tar);            /* TAR */
	emu_ap_write(0, 0x0c, 0x11111111u);    /* DRW -> [0x20000010], TAR+=4 */
	emu_ap_write(0, 0x0c, 0x22222222u);    /* DRW -> [0x20000014], TAR+=4 */
	uint32_t w0, w1;
	emu_mem_read(0x20000010u, &w0, 4);
	emu_mem_read(0x20000014u, &w1, 4);
	CHECK(w0 == 0x11111111u && w1 == 0x22222222u, "AP TAR/DRW auto-increment writes");

	/* 11. flash: unlock FPEC, page-erase, program a halfword, verify */
	uint32_t k1 = 0x45670123u, k2 = 0xcdef89abu;
	emu_mem_write(0x40022004u, &k1, 4, EMU_ACCESS_WORD); /* KEYR KEY1 */
	emu_mem_write(0x40022004u, &k2, 4, EMU_ACCESS_WORD); /* KEYR KEY2 */
	uint32_t per_ar = 0x08000000u;
	/* set PER, AR, STRT */
	uint32_t per = (1u<<1);
	emu_mem_write(0x40022010u, &per, 4, EMU_ACCESS_WORD);      /* CR = PER   */
	emu_mem_write(0x40022014u, &per_ar, 4, EMU_ACCESS_WORD);   /* AR = page  */
	uint32_t strt = (1u<<1)|(1u<<6);
	emu_mem_write(0x40022010u, &strt, 4, EMU_ACCESS_WORD);     /* CR=PER|STRT*/
	uint32_t erased;
	emu_mem_read(0x08000000u, &erased, 4);
	CHECK(erased == 0xffffffffu, "flash page erased -> 0x%08x", erased);
	/* program halfword 0xB00B at 0x08000000 */
	uint32_t pg = (1u<<0);
	emu_mem_write(0x40022010u, &pg, 4, EMU_ACCESS_WORD);       /* CR = PG    */
	uint16_t hw = 0xb00b;
	emu_mem_write(0x08000000u, &hw, 2, EMU_ACCESS_HALF);
	uint16_t prog;
	emu_mem_read(0x08000000u, &prog, 2);
	CHECK(prog == 0xb00b, "flash programmed halfword = 0x%04x", prog);

	printf("\n%s (%d failure%s)\n", fails ? "FAILURES" : "ALL PASS", fails, fails == 1 ? "" : "s");
	return fails ? 1 : 0;
}
