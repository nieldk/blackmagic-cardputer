/*
 * emu_target.c - byte-addressable model of an STM32F103 medium-density target
 * plus its ARM CoreSight debug infrastructure (ROM table, SCS) and FPEC.
 *
 * Flash is a sparse array of 1 KiB pages allocated on demand, so RAM tracks
 * the size of the image actually programmed rather than the full 128 KiB. The
 * FPEC (flash controller) models unlock, page erase (PER), mass erase (MER)
 * and program (PG) so GDB / on-device `flash` can erase, program and verify.
 *
 * Everything here answers memory loads/stores only; the ADIv5 DP/AP transport
 * lives in emu_shim.c.
 */
#include "emu_target.h"
#include <string.h>
#include <stdlib.h>

/* ---- flash: sparse 1 KiB pages ------------------------------------------ */
#define FLASH_PAGES (EMU_FLASH_SIZE / EMU_FLASH_PAGE) /* 128 */
static uint8_t *g_flash_page[FLASH_PAGES];

/* ---- SRAM: full 20 KiB backing ------------------------------------------ */
static uint8_t g_sram[EMU_SRAM_SIZE];

/* ---- debug registers BMP writes then reads back ------------------------- */
static uint32_t g_dhcsr;
static uint32_t g_demcr;
static uint32_t g_dcrsr;
static uint32_t g_dcrdr;
static uint32_t g_aircr;

/* ---- FPEC state --------------------------------------------------------- */
#define FPEC_KEYR (EMU_FPEC_BASE + 0x04U)
#define FPEC_SR   (EMU_FPEC_BASE + 0x0cU)
#define FPEC_CR   (EMU_FPEC_BASE + 0x10U)
#define FPEC_AR   (EMU_FPEC_BASE + 0x14U)
#define FPEC_OBR  (EMU_FPEC_BASE + 0x1cU)
#define FPEC_WRPR (EMU_FPEC_BASE + 0x20U)
#define FPEC_KEY1 0x45670123U
#define FPEC_KEY2 0xCDEF89ABU
#define CR_LOCK (1U << 7U)
#define CR_STRT (1U << 6U)
#define CR_MER  (1U << 2U)
#define CR_PER  (1U << 1U)
#define CR_PG   (1U << 0U)
#define SR_BSY  (1U << 0U)
#define SR_EOP  (1U << 5U)

static uint32_t g_flash_keyr;
static uint32_t g_flash_cr;
static uint32_t g_flash_sr;
static uint32_t g_flash_ar;
static bool g_flash_unlocked;

/* ---- CoreSight ID helpers ---------------------------------------------- *
 * adiv5_ap_read_id() reads 16 bytes and keeps bytes [0],[4],[8],[12] as the
 * four ID bytes. We therefore only need the low byte of each ID word. Both
 * CIDR and PIDR ID words are returned with the meaningful value in bits 7:0. */
#define CIDR_ROMTABLE 0xB105100DU /* class 0x1 (ROM table)          */
#define CIDR_GIPC     0xB105E00DU /* class 0xe (Generic IP: SCS)    */

/* ROM table: part 0x4c3, designer STMicro (0x020). */
#define PIDR_ROM_LO 0x000A04C3U
#define PIDR_ROM_HI 0x00000000U
/* SCS: part 0x000, designer ARM (0x43b). */
#define PIDR_SCS_LO 0x000BB000U
#define PIDR_SCS_HI 0x00000004U

static uint32_t id_word(uint32_t cidr, uint32_t pidr_lo, uint32_t pidr_hi, uint32_t off)
{
	switch (off) {
	case 0xFF0U:
		return (cidr >> 0U) & 0xffU;
	case 0xFF4U:
		return (cidr >> 8U) & 0xffU;
	case 0xFF8U:
		return (cidr >> 16U) & 0xffU;
	case 0xFFCU:
		return (cidr >> 24U) & 0xffU;
	case 0xFE0U:
		return (pidr_lo >> 0U) & 0xffU;
	case 0xFE4U:
		return (pidr_lo >> 8U) & 0xffU;
	case 0xFE8U:
		return (pidr_lo >> 16U) & 0xffU;
	case 0xFECU:
		return (pidr_lo >> 24U) & 0xffU;
	case 0xFD0U:
		return (pidr_hi >> 0U) & 0xffU;
	default:
		return 0U;
	}
}

/* ---- ROM table ---------------------------------------------------------- */
#define ROM_ENTRY_SCS 0xFFF0F003U /* -> SCS @ 0xE000E000, present */
#define ROM_MEMTYPE   0xFCCU      /* SYSMEM bit0 */

static uint32_t rom_read(uint32_t off)
{
	if (off >= 0xFD0U && off <= 0xFFCU)
		return id_word(CIDR_ROMTABLE, PIDR_ROM_LO, PIDR_ROM_HI, off);
	if (off == ROM_MEMTYPE)
		return 0x1U;
	switch (off) {
	case 0x000U:
		return ROM_ENTRY_SCS;
	case 0x004U:
		return 0x00000000U; /* end of table */
	default:
		return 0x00000000U;
	}
}

/* ---- SCS / Cortex-M3 debug block ---------------------------------------- *
 * Core registers are transferred via DCRSR (select + direction) and DCRDR
 * (data), exactly as cortexm.c drives them. regnum encoding: 0..15 = r0..r15
 * (13=sp, 14=lr, 15=pc), 0x10 = xPSR, 0x11 = MSP, 0x12 = PSP, 0x14 = the
 * packed CONTROL/FAULTMASK/BASEPRI/PRIMASK special register.               */
#define CORE_REG_COUNT 0x18U
static uint32_t g_core_reg[CORE_REG_COUNT];
#define DCRSR_REGWnR 0x00010000U

static uint32_t scs_read(uint32_t off)
{
	if (off >= 0xFD0U && off <= 0xFFCU)
		return id_word(CIDR_GIPC, PIDR_SCS_LO, PIDR_SCS_HI, off);
	switch (off) {
	case 0xD00U: /* CPUID */
		return EMU_CPUID;
	case 0xD88U: /* CPACR - report no FP (readback != written) => v7m */
		return 0x00000000U;
	case 0xDF0U: /* DHCSR - halted, debug enabled, regs ready, no reset */
		return 0x00030003U;
	case 0xDF4U: /* DCRSR */
		return g_dcrsr;
	case 0xDF8U: /* DCRDR - holds the selected register's value after a read */
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
	case 0xDF0U:
		g_dhcsr = value;
		break;
	case 0xDF4U: { /* DCRSR: perform the register transfer */
		g_dcrsr = value;
		const uint32_t regnum = value & 0x7fU;
		if (regnum < CORE_REG_COUNT) {
			if (value & DCRSR_REGWnR) /* write direction: DCRDR -> reg */
				g_core_reg[regnum] = g_dcrdr;
			else /* read direction: reg -> DCRDR */
				g_dcrdr = g_core_reg[regnum];
		}
		break;
	}
	case 0xDF8U: /* DCRDR */
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

/* ---- flash page helpers ------------------------------------------------- */
static uint8_t flash_byte(uint32_t addr)
{
	uint32_t off = addr - EMU_FLASH_BASE;
	uint32_t page = off / EMU_FLASH_PAGE;
	if (page >= FLASH_PAGES || !g_flash_page[page])
		return 0xFFU; /* erased */
	return g_flash_page[page][off % EMU_FLASH_PAGE];
}

static void flash_write_byte(uint32_t addr, uint8_t value)
{
	uint32_t off = addr - EMU_FLASH_BASE;
	uint32_t page = off / EMU_FLASH_PAGE;
	if (page >= FLASH_PAGES)
		return;
	if (!g_flash_page[page]) {
		g_flash_page[page] = malloc(EMU_FLASH_PAGE);
		if (!g_flash_page[page])
			return; /* out of RAM: silently drop (verify will catch) */
		memset(g_flash_page[page], 0xFF, EMU_FLASH_PAGE);
	}
	/* NOR flash can only clear bits without an erase; AND models that. */
	g_flash_page[page][off % EMU_FLASH_PAGE] &= value;
}

static void flash_erase_page(uint32_t addr)
{
	uint32_t off = addr - EMU_FLASH_BASE;
	uint32_t page = off / EMU_FLASH_PAGE;
	if (page >= FLASH_PAGES)
		return;
	if (g_flash_page[page]) {
		free(g_flash_page[page]);
		g_flash_page[page] = NULL; /* absent page reads as 0xFF */
	}
}

static void flash_erase_all(void)
{
	for (uint32_t i = 0; i < FLASH_PAGES; ++i) {
		free(g_flash_page[i]);
		g_flash_page[i] = NULL;
	}
}

/* ---- FPEC --------------------------------------------------------------- */
static uint32_t fpec_read(uint32_t addr)
{
	switch (addr) {
	case FPEC_SR:
		return g_flash_sr; /* BSY never set: operations are instant */
	case FPEC_CR:
		/* LOCK reflects unlock state; mode bits reflect last write. */
		return (g_flash_cr & ~CR_LOCK) | (g_flash_unlocked ? 0U : CR_LOCK);
	case FPEC_OBR:
		return 0x00000000U; /* not read-protected */
	case FPEC_WRPR:
		return 0xFFFFFFFFU; /* no write protection */
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
		if ((value & CR_STRT) && g_flash_unlocked) {
			if (value & CR_PER)
				flash_erase_page(g_flash_ar);
			else if (value & CR_MER)
				flash_erase_all();
			g_flash_sr |= SR_EOP;
			g_flash_sr &= ~SR_BSY;
		}
		break;
	case FPEC_AR:
		g_flash_ar = value;
		break;
	case FPEC_SR:
		g_flash_sr &= ~(value & SR_EOP); /* EOP is W1C */
		break;
	default:
		break;
	}
}

/* ---- public dispatch ---------------------------------------------------- */
static uint32_t size_mask(uint32_t size)
{
	return size >= 4U ? 0xFFFFFFFFU : ((1U << (size * 8U)) - 1U);
}

uint32_t emu_target_load(uint32_t addr, uint32_t size)
{
	const uint32_t mask = size_mask(size);

	if (addr >= EMU_FLASH_BASE && addr < EMU_FLASH_BASE + EMU_FLASH_SIZE) {
		uint32_t v = 0;
		for (uint32_t i = 0; i < size; ++i)
			v |= (uint32_t)flash_byte(addr + i) << (8U * i);
		return v & mask;
	}
	if (addr >= EMU_SRAM_BASE && addr < EMU_SRAM_BASE + EMU_SRAM_SIZE) {
		uint32_t v = 0;
		for (uint32_t i = 0; i < size; ++i)
			v |= (uint32_t)g_sram[(addr + i) - EMU_SRAM_BASE] << (8U * i);
		return v & mask;
	}
	if (addr == EMU_DBGMCU_IDCODE)
		return EMU_IDCODE & mask;
	if (addr >= EMU_ROM_BASE && addr < EMU_ROM_BASE + 0x1000U)
		return rom_read(addr - EMU_ROM_BASE) & mask;
	if (addr >= EMU_SCS_BASE && addr < EMU_SCS_BASE + 0x1000U)
		return scs_read(addr - EMU_SCS_BASE) & mask;
	if (addr >= EMU_FPEC_BASE && addr < EMU_FPEC_BASE + 0x400U)
		return fpec_read(addr) & mask;
	return 0x00000000U;
}

void emu_target_store(uint32_t addr, uint32_t value, uint32_t size)
{
	if (addr >= EMU_FLASH_BASE && addr < EMU_FLASH_BASE + EMU_FLASH_SIZE) {
		if (g_flash_unlocked && (g_flash_cr & CR_PG)) {
			for (uint32_t i = 0; i < size; ++i)
				flash_write_byte(addr + i, (uint8_t)(value >> (8U * i)));
			g_flash_sr |= SR_EOP; /* program complete */
			g_flash_sr &= ~SR_BSY;
		}
		return;
	}
	if (addr >= EMU_SRAM_BASE && addr < EMU_SRAM_BASE + EMU_SRAM_SIZE) {
		for (uint32_t i = 0; i < size; ++i)
			g_sram[(addr + i) - EMU_SRAM_BASE] = (uint8_t)(value >> (8U * i));
		return;
	}
	if (addr >= EMU_SCS_BASE && addr < EMU_SCS_BASE + 0x1000U) {
		scs_write(addr - EMU_SCS_BASE, value);
		return;
	}
	if (addr >= EMU_FPEC_BASE && addr < EMU_FPEC_BASE + 0x400U) {
		fpec_write(addr, value);
		return;
	}
	/* stores elsewhere are silently dropped */
}

uint32_t emu_target_read_word(uint32_t addr)
{
	return emu_target_load(addr, 4U);
}

void emu_target_write_word(uint32_t addr, uint32_t value)
{
	emu_target_store(addr, value, 4U);
}

void emu_target_reset(void)
{
	flash_erase_all();
	memset(g_sram, 0x00, sizeof(g_sram));

	/* Seed a plausible vector table so `x/4xw 0x08000000` looks real,
	 * written directly (bypassing FPEC lock). */
	g_flash_page[0] = malloc(EMU_FLASH_PAGE);
	if (g_flash_page[0]) {
		memset(g_flash_page[0], 0xFF, EMU_FLASH_PAGE);
		uint32_t sp = 0x20005000U;  /* top of 20 KiB SRAM */
		uint32_t rst = 0x08000101U; /* reset handler, thumb bit set */
		memcpy(g_flash_page[0] + 0, &sp, 4);
		memcpy(g_flash_page[0] + 4, &rst, 4);
	}

	g_dhcsr = 0;
	g_demcr = 0;
	g_dcrsr = 0;
	g_dcrdr = 0;
	g_aircr = 0;
	g_flash_keyr = 0;
	g_flash_cr = 0;
	g_flash_sr = 0;
	g_flash_ar = 0;
	g_flash_unlocked = false;

	/* Seed a plausible halted-at-reset core state. GDB / cortexm may
	 * overwrite these (e.g. pc after `load`) via the DCRSR/DCRDR path. */
	memset(g_core_reg, 0, sizeof(g_core_reg));
	g_core_reg[13] = 0x20005000U; /* sp   = top of SRAM         */
	g_core_reg[14] = 0xFFFFFFFFU; /* lr                          */
	g_core_reg[15] = 0x08000100U; /* pc   = reset handler (even) */
	g_core_reg[0x10] = 0x01000000U; /* xPSR = Thumb (T) bit set  */
	g_core_reg[0x11] = 0x20005000U; /* MSP                        */
}