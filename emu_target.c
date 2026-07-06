/*
 * emu_target.c - word-addressed model of an STM32F103 medium-density target
 * plus its ARM CoreSight debug infrastructure (ROM table, SCS, FPEC).
 *
 * Everything here answers memory reads/writes only; the ADIv5 DP/AP transport
 * lives in emu_shim.c. The layout is exactly what BMP's adiv5_component_probe,
 * cortexm_probe and stm32f1_probe walk, so target detection succeeds without
 * any physical SWD.
 */
#include "emu_target.h"
#include <string.h>

/* ---- backing stores ----------------------------------------------------- *
 * Kept small: only the first pages are backed. Reads outside the backed
 * window return the erased/zero value for the region. This is enough for
 * detection and for inspecting the vector table; it is NOT a CPU model.     */
#define EMU_FLASH_BACKED 0x400U /* 1 KiB seeded (vector table + a little)   */
#define EMU_SRAM_BACKED  0x400U /* 1 KiB seeded                             */

static uint8_t g_flash[EMU_FLASH_BACKED];
static uint8_t g_sram[EMU_SRAM_BACKED];

/* Debug registers that BMP writes then reads back. */
static uint32_t g_dhcsr;
static uint32_t g_demcr;
static uint32_t g_dcrsr;
static uint32_t g_dcrdr;
static uint32_t g_aircr;

/* FPEC (flash controller) minimal state, for the erase/program paths. */
static uint32_t g_flash_keyr;
static uint32_t g_flash_cr;
static uint32_t g_flash_sr;
static bool g_flash_unlocked;

#define FPEC_BASE 0x40022000U
#define FPEC_KEYR (FPEC_BASE + 0x04U)
#define FPEC_SR   (FPEC_BASE + 0x0cU)
#define FPEC_CR   (FPEC_BASE + 0x10U)
#define FPEC_AR   (FPEC_BASE + 0x14U)
#define FPEC_KEY1 0x45670123U
#define FPEC_KEY2 0xCDEF89ABU
#define FPEC_SR_BSY 0x01U
#define FPEC_SR_EOP 0x20U

/* ---- CoreSight ID helpers ---------------------------------------------- *
 * adiv5_ap_read_id() reads 16 bytes and keeps bytes [0],[4],[8],[12] as the
 * four ID bytes. We therefore only need the low byte of each ID word. Both
 * CIDR and PIDR ID words are returned with the meaningful value in bits 7:0. */

/* Component ID (CIDR0..3 low bytes -> assembled 32-bit). */
#define CIDR_ROMTABLE 0xB105100DU /* class 0x1 (ROM table)          */
#define CIDR_GIPC     0xB105E00DU /* class 0xe (Generic IP: SCS)    */

/* Peripheral ID low/high words. Only low byte of each 4-byte slot is used. */
/* ROM table: part 0x4c3, designer STMicro (0x020). */
#define PIDR_ROM_LO 0x000A04C3U /* PIDR0..3 assembled */
#define PIDR_ROM_HI 0x00000000U /* PIDR4 (JEP106 continuation 0x0) */
/* SCS: part 0x000, designer ARM (0x43b). */
#define PIDR_SCS_LO 0x000BB000U /* PIDR0..3 assembled */
#define PIDR_SCS_HI 0x00000004U /* PIDR4 (JEP106 continuation 0x4) */

/* Return the ID byte for a given component-relative offset in the
 * 0xFD0..0xFFC ID window. off is masked to that window by the caller. */
static uint32_t id_word(uint32_t cidr, uint32_t pidr_lo, uint32_t pidr_hi, uint32_t off)
{
	switch (off) {
	/* CIDR0..3 at 0xFF0,0xFF4,0xFF8,0xFFC */
	case 0xFF0U:
		return (cidr >> 0U) & 0xffU;
	case 0xFF4U:
		return (cidr >> 8U) & 0xffU;
	case 0xFF8U:
		return (cidr >> 16U) & 0xffU;
	case 0xFFCU:
		return (cidr >> 24U) & 0xffU;
	/* PIDR0..3 at 0xFE0,0xFE4,0xFE8,0xFEC */
	case 0xFE0U:
		return (pidr_lo >> 0U) & 0xffU;
	case 0xFE4U:
		return (pidr_lo >> 8U) & 0xffU;
	case 0xFE8U:
		return (pidr_lo >> 16U) & 0xffU;
	case 0xFECU:
		return (pidr_lo >> 24U) & 0xffU;
	/* PIDR4..7 at 0xFD0,0xFD4,0xFD8,0xFDC */
	case 0xFD0U:
		return (pidr_hi >> 0U) & 0xffU;
	case 0xFD4U:
	case 0xFD8U:
	case 0xFDCU:
		return 0U;
	default:
		return 0U;
	}
}

/* ---- ROM table ---------------------------------------------------------- *
 * Minimal: one present entry pointing at the SCS, then a terminator.
 * Offset for SCS: 0xE000E000 - 0xE00FF000 = 0xFFF0F000 (wraps), +present.   */
#define ROM_ENTRY_SCS 0xFFF0F003U
#define ROM_MEMTYPE   0xFCCU /* SYSMEM bit0 */

static uint32_t rom_read(uint32_t off)
{
	if (off >= 0xFD0U && off <= 0xFFCU)
		return id_word(CIDR_ROMTABLE, PIDR_ROM_LO, PIDR_ROM_HI, off);
	if (off == ROM_MEMTYPE)
		return 0x1U; /* SYSMEM present */
	switch (off) {
	case 0x000U:
		return ROM_ENTRY_SCS;
	case 0x004U:
		return 0x00000000U; /* end of table */
	default:
		return 0x00000000U;
	}
}

/* ---- SCS / Cortex-M3 debug block ---------------------------------------- */
static uint32_t scs_read(uint32_t off)
{
	if (off >= 0xFD0U && off <= 0xFFCU)
		return id_word(CIDR_GIPC, PIDR_SCS_LO, PIDR_SCS_HI, off);
	switch (off) {
	case 0xD00U: /* CPUID */
		return EMU_CPUID;
	case 0xD88U: /* CPACR - report no FP (readback != written) => v7m */
		return 0x00000000U;
	case 0xDF0U: /* DHCSR - halted, debug enabled, regs ready, no reset  */
		return 0x00030003U;
	case 0xDF4U: /* DCRSR */
		return g_dcrsr;
	case 0xDF8U: /* DCRDR */
		return g_dcrdr;
	case 0xDFCU: /* DEMCR */
		return g_demcr;
	case 0xD0CU: /* AIRCR */
		return g_aircr;
	default:
		return 0x00000000U;
	}
}

static void scs_write(uint32_t off, uint32_t value)
{
	switch (off) {
	case 0xDF0U: /* DHCSR */
		g_dhcsr = value;
		break;
	case 0xDF4U:
		g_dcrsr = value;
		break;
	case 0xDF8U:
		g_dcrdr = value;
		break;
	case 0xDFCU:
		g_demcr = value;
		break;
	case 0xD0CU:
		g_aircr = value;
		break;
	default:
		break;
	}
}

/* ---- FPEC --------------------------------------------------------------- */
static uint32_t fpec_read(uint32_t addr)
{
	switch (addr) {
	case FPEC_SR:
		return g_flash_sr; /* BSY never set: operations are instant */
	case FPEC_CR:
		return g_flash_cr;
	default:
		return 0x00000000U;
	}
}

static void fpec_write(uint32_t addr, uint32_t value)
{
	switch (addr) {
	case FPEC_KEYR:
		if (g_flash_keyr == FPEC_KEY1 && value == FPEC_KEY2)
			g_flash_unlocked = true;
		g_flash_keyr = value;
		break;
	case FPEC_CR:
		g_flash_cr = value;
		/* Signal completion immediately for erase/program ops. */
		g_flash_sr |= FPEC_SR_EOP;
		g_flash_sr &= ~FPEC_SR_BSY;
		break;
	case FPEC_SR:
		/* write-1-clear on EOP (and WRPRTERR/PGERR which we don't model) */
		g_flash_sr &= ~(value & FPEC_SR_EOP);
		break;
	default:
		break;
	}
}

/* ---- flash / SRAM ------------------------------------------------------- */
static uint32_t mem_backed_read(const uint8_t *store, uint32_t backed, uint32_t off, uint32_t erased)
{
	if (off + 4U <= backed) {
		uint32_t v;
		memcpy(&v, store + off, sizeof(v));
		return v;
	}
	return erased;
}

/* ---- public dispatch ---------------------------------------------------- */
uint32_t emu_target_read_word(uint32_t addr)
{
	if (addr >= EMU_FLASH_BASE && addr < EMU_FLASH_BASE + EMU_FLASH_SIZE)
		return mem_backed_read(g_flash, EMU_FLASH_BACKED, addr - EMU_FLASH_BASE, 0xFFFFFFFFU);
	if (addr >= EMU_SRAM_BASE && addr < EMU_SRAM_BASE + EMU_SRAM_SIZE)
		return mem_backed_read(g_sram, EMU_SRAM_BACKED, addr - EMU_SRAM_BASE, 0x00000000U);
	if (addr == EMU_DBGMCU_IDCODE)
		return EMU_IDCODE;
	if (addr >= EMU_ROM_BASE && addr < EMU_ROM_BASE + 0x1000U)
		return rom_read(addr - EMU_ROM_BASE);
	if (addr >= EMU_SCS_BASE && addr < EMU_SCS_BASE + 0x1000U)
		return scs_read(addr - EMU_SCS_BASE);
	if (addr >= FPEC_BASE && addr < FPEC_BASE + 0x400U)
		return fpec_read(addr);
	return 0x00000000U;
}

void emu_target_write_word(uint32_t addr, uint32_t value)
{
	if (addr >= EMU_FLASH_BASE && addr < EMU_FLASH_BASE + EMU_FLASH_SIZE) {
		uint32_t off = addr - EMU_FLASH_BASE;
		if (g_flash_unlocked && off + 4U <= EMU_FLASH_BACKED)
			memcpy(g_flash + off, &value, sizeof(value));
		return;
	}
	if (addr >= EMU_SRAM_BASE && addr < EMU_SRAM_BASE + EMU_SRAM_SIZE) {
		uint32_t off = addr - EMU_SRAM_BASE;
		if (off + 4U <= EMU_SRAM_BACKED)
			memcpy(g_sram + off, &value, sizeof(value));
		return;
	}
	if (addr >= EMU_SCS_BASE && addr < EMU_SCS_BASE + 0x1000U) {
		scs_write(addr - EMU_SCS_BASE, value);
		return;
	}
	if (addr >= FPEC_BASE && addr < FPEC_BASE + 0x400U) {
		fpec_write(addr, value);
		return;
	}
	/* writes elsewhere are silently dropped */
}

void emu_target_reset(void)
{
	memset(g_flash, 0xFF, sizeof(g_flash));
	memset(g_sram, 0x00, sizeof(g_sram));
	/* Seed a plausible vector table so `x/4xw 0x08000000` looks real. */
	uint32_t sp = 0x20005000U; /* top of 20 KiB SRAM */
	uint32_t rst = 0x08000101U; /* reset handler, thumb bit set */
	memcpy(g_flash + 0, &sp, 4);
	memcpy(g_flash + 4, &rst, 4);
	g_dhcsr = 0;
	g_demcr = 0;
	g_dcrsr = 0;
	g_dcrdr = 0;
	g_aircr = 0;
	g_flash_keyr = 0;
	g_flash_cr = 0;
	g_flash_sr = 0;
	g_flash_unlocked = false;
}
