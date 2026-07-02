/*
 * emu_target.c - in-firmware emulated ADIv5 SW-DP + Cortex-M debug target.
 * See emu_target.h for the contract. Models an STM32F103 well enough for BMP
 * to scan, attach, read/write memory and program flash. The core never runs;
 * it reports as permanently halted.
 */
#include "emu_target.h"
#include <string.h>

/* ======================================================================== *
 *  IDENTIFICATION CONSTANTS  --  VERIFY AGAINST YOUR TREE
 *
 *  Every value in this block is what BMP fingerprints the target by. They are
 *  the standard ARM / STM32F103 values, but the scan only succeeds if they
 *  match the tables your adiv5.c actually checks (arm_component_lut[], and the
 *  DPIDR / AP-IDR handling). Send me adiv5.c and I will lock these to your LUT.
 * ======================================================================== */
#define EMU_DPIDR         0x1ba01477u /* STM32F1 SW-DP IDCODE                */
#define EMU_AP_IDR        0x14770011u /* AHB-AP identification (STM32F1)     */
#define EMU_AP_CFG        0x00000000u /* 32-bit, little-endian, no large addr*/
#define EMU_AP_BASE       0xe00ff003u /* ROM table @0xE00FF000, present+fmt  */
#define EMU_CPUID         0x412fc231u /* Cortex-M3 r2p1 (part 0xC23)         */
#define EMU_DBGMCU_IDCODE 0x20036410u /* STM32F103xB: DEV_ID 0x410, REV 0x2003 */

/* Confirmed against your adiv5.c arm_component_lut:
 *   {0x4c3, ..., "Cortex-M3 ROM"}                 -> ROM table part number
 *   {0x000, ..., aa_cortexm, cidc_gipc, "SCS"}    -> SCS: part 0x000, class 0xe
 * cortexm_probe then dispatches on designer ARM + part 0x4c3 -> stm32f1_probe.
 * The SCS component MUST be class 0xe (Generic IP Component), not 0x9, or the
 * probe reads DEVTYPE/DEVARCH and logs a cidc mismatch. */
#define ROM_PART          0x4c3u
#define ROM_CID           0xb105100du /* class 0x1 = ROM table                */
#define CS_CID            0xb105e00du /* class 0xe = cidc_gipc (matches SCS)  */

/* ======================================================================== *
 *  Emulated memory sizes (STM32F103C8 defaults). Each byte here costs one
 *  byte of ESP32-S3 SRAM. Shrink for tighter builds; grow for larger parts.
 * ======================================================================== */
/* STM32F103xB medium density: stm32f1_probe hardcodes 128 KiB flash / 20 KiB
 * RAM for device id 0x410, so back the full region to match. ~148 KiB of
 * ESP32-S3 SRAM; shrink both for a smaller emulated part if space is tight. */
#ifndef EMU_FLASH_SIZE
#define EMU_FLASH_SIZE (128u * 1024u) /* 0x08000000..                       */
#endif
#ifndef EMU_SRAM_SIZE
#define EMU_SRAM_SIZE (20u * 1024u) /* 0x20000000..                         */
#endif

#define FLASH_BASE 0x08000000u
#define SRAM_BASE  0x20000000u

/* ---- Cortex-M / STM32 register addresses -------------------------------- */
#define SCS_BASE       0xe000e000u
#define CPUID_ADDR     0xe000ed00u
#define AIRCR_ADDR     0xe000ed0cu
#define DHCSR_ADDR     0xe000edf0u
#define DCRSR_ADDR     0xe000edf4u
#define DCRDR_ADDR     0xe000edf8u
#define DEMCR_ADDR     0xe000edfcu
#define ROM_BASE       0xe00ff000u
#define DBGMCU_IDCODE  0xe0042000u

/* DHCSR bits */
#define DHCSR_DBGKEY   0xa05f0000u
#define DHCSR_C_DEBUGEN (1u << 0)
#define DHCSR_C_HALT    (1u << 1)
#define DHCSR_S_REGRDY  (1u << 16)
#define DHCSR_S_HALT    (1u << 17)

/* AIRCR */
#define AIRCR_VECTKEY   0x05fa0000u
#define AIRCR_SYSRESETREQ (1u << 2)
#define AIRCR_VECTRESET   (1u << 0)

/* FPEC (flash program/erase controller) */
#define FPEC_BASE      0x40022000u
#define FPEC_ACR       (FPEC_BASE + 0x00u)
#define FPEC_KEYR      (FPEC_BASE + 0x04u)
#define FPEC_OPTKEYR   (FPEC_BASE + 0x08u)
#define FPEC_SR        (FPEC_BASE + 0x0cu)
#define FPEC_CR        (FPEC_BASE + 0x10u)
#define FPEC_AR        (FPEC_BASE + 0x14u)
#define FPEC_OBR       (FPEC_BASE + 0x1cu)
#define FPEC_WRPR      (FPEC_BASE + 0x20u)
#define FPEC_KEY1      0x45670123u
#define FPEC_KEY2      0xcdef89abu
#define FPEC_CR_PG     (1u << 0)
#define FPEC_CR_PER    (1u << 1)
#define FPEC_CR_MER    (1u << 2)
#define FPEC_CR_STRT   (1u << 6)
#define FPEC_CR_LOCK   (1u << 7)
#define FPEC_SR_BSY    (1u << 0)
#define FPEC_SR_EOP    (1u << 5)
#define FLASH_PAGE_SIZE 1024u

/* DP CTRL/STAT power/reset request<->ack mirroring */
#define CTRL_CDBGPWRUPREQ  (1u << 28)
#define CTRL_CDBGPWRUPACK  (1u << 29)
#define CTRL_CSYSPWRUPREQ  (1u << 30)
#define CTRL_CSYSPWRUPACK  (1u << 31)
#define CTRL_CDBGRSTREQ    (1u << 26)
#define CTRL_CDBGRSTACK    (1u << 27)
#define CTRL_STICKY_MASK   0x000000b2u /* STICKYERR|STICKYCMP|STICKYORUN|WDATAERR */

/* ======================================================================== *
 *  State
 * ======================================================================== */
static bool     g_enabled;
static uint8_t  g_flash[EMU_FLASH_SIZE];
static uint8_t  g_sram[EMU_SRAM_SIZE];

/* DP */
static uint32_t g_dp_select;
static uint32_t g_dp_ctrlstat;
static uint32_t g_rdbuff;

/* AP0 (MEM-AP) */
static uint32_t g_ap_csw;
static uint32_t g_ap_tar;

/* Cortex-M debug */
static uint32_t g_regfile[24]; /* r0-r15, xPSR, MSP, PSP, CONTROL, ...       */
static uint32_t g_dhcsr;
static uint32_t g_dcrsr;
static uint32_t g_dcrdr;
static uint32_t g_demcr;

/* FPEC */
static uint32_t g_flash_cr = FPEC_CR_LOCK;
static uint32_t g_flash_sr;
static uint32_t g_flash_ar;
static uint32_t g_flash_acr;
static uint8_t  g_flash_key_state; /* 0=locked, 1=KEY1 seen, 2=unlocked      */

static inline uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* ------------------------------------------------------------------------ *
 *  Reset / init
 * ------------------------------------------------------------------------ */
static void core_reset(void)
{
	memset(g_regfile, 0, sizeof(g_regfile));
	/* SP (r13) and PC (r15) from the flash vector table, if programmed. */
	g_regfile[13] = rd32(&g_flash[0]);
	g_regfile[15] = rd32(&g_flash[4]) & ~1u;
	g_dhcsr = DHCSR_S_HALT | DHCSR_S_REGRDY; /* always halted */
	g_demcr = 0;
	g_flash_cr = FPEC_CR_LOCK;
	g_flash_sr = 0;
	g_flash_key_state = 0;
}

void emu_target_init(void)
{
	memset(g_flash, 0xff, sizeof(g_flash)); /* erased flash reads as 0xFF */
	memset(g_sram, 0, sizeof(g_sram));
	g_dp_select = 0;
	g_dp_ctrlstat = 0;
	g_ap_csw = 0x03000012u; /* 32-bit, addr-inc single, DbgSwEnable defaults */
	g_ap_tar = 0;
	g_flash_acr = 0;
	core_reset();
	g_enabled = true;
}

bool emu_target_enabled(void) { return g_enabled; }
void emu_target_set_enabled(bool on) { g_enabled = on; }

/* ------------------------------------------------------------------------ *
 *  ROM table + CoreSight component ID space
 *  Serves the ROM table at 0xE00FF000 and the CIDR/PIDR of each component so
 *  BMP's ROM-table walk can enumerate the debug block and reach the SCS.
 * ------------------------------------------------------------------------ */
static uint32_t rom_table_read(uint32_t addr)
{
	uint32_t off = addr - ROM_BASE;
	switch (off) {
	/* entries: (component_base - ROM_BASE) with present(0)+format(1) bits */
	case 0x000: return ((SCS_BASE - ROM_BASE) & 0xfffff000u) | 3u; /* SCS  */
	case 0x004: return ((0xe0001000u - ROM_BASE) & 0xfffff000u) | 3u; /* DWT */
	case 0x008: return ((0xe0002000u - ROM_BASE) & 0xfffff000u) | 3u; /* FPB */
	case 0x00c: return ((0xe0000000u - ROM_BASE) & 0xfffff000u) | 3u; /* ITM */
	case 0x010: return 0x00000000u; /* end of table                        */
	case 0xfcc: return 0x00000001u; /* MEMTYPE: system memory present       */
	/* peripheral + component ID of the ROM table itself */
	case 0xfd0: return 0x04; case 0xfd4: return 0x00; case 0xfd8: return 0x00; case 0xfdc: return 0x00;
	case 0xfe0: return (ROM_PART & 0xffu);           /* PIDR0 part[7:0]     */
	case 0xfe4: return 0xb0u | ((ROM_PART >> 8) & 0xfu); /* PIDR1 part[11:8]+des */
	case 0xfe8: return 0x0bu;                         /* PIDR2 des+jedec     */
	case 0xfec: return 0x00u;                         /* PIDR3               */
	case 0xff0: return (ROM_CID >> 0) & 0xff;
	case 0xff4: return (ROM_CID >> 8) & 0xff;
	case 0xff8: return (ROM_CID >> 16) & 0xff;
	case 0xffc: return (ROM_CID >> 24) & 0xff;
	default: return 0;
	}
}

/* CIDR/PIDR for a CoreSight component (SCS/DWT/FPB/ITM). Part numbers are the
 * standard Cortex-M3 values; adjust to match your LUT if a component is not
 * recognised during the walk. */
static uint32_t component_id_read(uint32_t base, uint32_t addr, uint16_t part)
{
	uint32_t off = addr - base;
	switch (off) {
	case 0xfd0: return 0x04;
	case 0xfe0: return part & 0xffu;
	case 0xfe4: return 0xb0u | ((part >> 8) & 0xfu);
	case 0xfe8: return 0x0bu;
	case 0xfec: return 0x00u;
	case 0xff0: return (CS_CID >> 0) & 0xff;
	case 0xff4: return (CS_CID >> 8) & 0xff;
	case 0xff8: return (CS_CID >> 16) & 0xff;
	case 0xffc: return (CS_CID >> 24) & 0xff;
	default: return 0;
	}
}

/* ------------------------------------------------------------------------ *
 *  SCS debug registers (CPUID, DHCSR, DCRSR/DCRDR reg file, DEMCR, AIRCR)
 * ------------------------------------------------------------------------ */
static uint32_t scs_read(uint32_t addr)
{
	switch (addr) {
	case CPUID_ADDR: return EMU_CPUID;
	case DHCSR_ADDR: return g_dhcsr | DHCSR_S_HALT | DHCSR_S_REGRDY;
	case DCRSR_ADDR: return g_dcrsr;
	case DCRDR_ADDR: return g_dcrdr;
	case DEMCR_ADDR: return g_demcr;
	case AIRCR_ADDR: return AIRCR_VECTKEY; /* reads back with key field */
	default: break;
	}
	/* SCS component ID block (0xE000EFD0..0xE000EFFC), part 0x000/0xC23-ish */
	if (addr >= SCS_BASE + 0xfd0u)
		return component_id_read(SCS_BASE, addr, 0x000);
	return 0;
}

static void scs_write(uint32_t addr, uint32_t value)
{
	switch (addr) {
	case DHCSR_ADDR:
		if ((value & 0xffff0000u) == DHCSR_DBGKEY) {
			/* keep control bits; core stays halted in this model */
			g_dhcsr = (value & 0x0000000fu) | DHCSR_S_HALT | DHCSR_S_REGRDY;
		}
		break;
	case DCRSR_ADDR: {
		g_dcrsr = value;
		uint32_t idx = value & 0x7fu;
		bool wnr = (value & (1u << 16)) != 0;
		if (idx < 24u) {
			if (wnr)
				g_regfile[idx] = g_dcrdr;   /* write core reg   */
			else
				g_dcrdr = g_regfile[idx];   /* read core reg    */
		}
		g_dhcsr |= DHCSR_S_REGRDY;
		break;
	}
	case DCRDR_ADDR:
		g_dcrdr = value;
		break;
	case DEMCR_ADDR:
		g_demcr = value;
		break;
	case AIRCR_ADDR:
		if ((value & 0xffff0000u) == AIRCR_VECTKEY &&
		    (value & (AIRCR_SYSRESETREQ | AIRCR_VECTRESET)))
			core_reset();
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------------ *
 *  FPEC flash controller
 * ------------------------------------------------------------------------ */
static uint32_t fpec_read(uint32_t addr)
{
	switch (addr) {
	case FPEC_ACR: return g_flash_acr;
	case FPEC_SR:  return g_flash_sr;              /* BSY never set: instant */
	case FPEC_CR:  return g_flash_cr;
	case FPEC_AR:  return g_flash_ar;
	case FPEC_OBR: return 0x03fffffcu;             /* option bytes, RDP off  */
	case FPEC_WRPR: return 0xffffffffu;            /* no write protection    */
	default: return 0;
	}
}

static void fpec_write(uint32_t addr, uint32_t value)
{
	switch (addr) {
	case FPEC_ACR:
		g_flash_acr = value;
		break;
	case FPEC_KEYR:
		if (g_flash_key_state == 0 && value == FPEC_KEY1)
			g_flash_key_state = 1;
		else if (g_flash_key_state == 1 && value == FPEC_KEY2) {
			g_flash_key_state = 2;
			g_flash_cr &= ~FPEC_CR_LOCK;           /* unlocked          */
		} else
			g_flash_key_state = 0;
		break;
	case FPEC_CR:
		g_flash_cr = value;
		if (value & FPEC_CR_LOCK) {
			g_flash_key_state = 0;
			g_flash_cr |= FPEC_CR_LOCK;
		}
		if ((value & FPEC_CR_STRT) && !(g_flash_cr & FPEC_CR_LOCK)) {
			if (value & FPEC_CR_MER) {            /* mass erase        */
				memset(g_flash, 0xff, sizeof(g_flash));
				g_flash_sr |= FPEC_SR_EOP;
			} else if (value & FPEC_CR_PER) {     /* page erase @AR    */
				uint32_t pa = g_flash_ar;
				if (pa >= FLASH_BASE && pa < FLASH_BASE + EMU_FLASH_SIZE) {
					uint32_t base = (pa - FLASH_BASE) & ~(FLASH_PAGE_SIZE - 1u);
					uint32_t n = FLASH_PAGE_SIZE;
					if (base + n > EMU_FLASH_SIZE)
						n = EMU_FLASH_SIZE - base;
					memset(&g_flash[base], 0xff, n);
				}
				g_flash_sr |= FPEC_SR_EOP;
			}
		}
		break;
	case FPEC_AR:
		g_flash_ar = value;
		break;
	case FPEC_SR:
		/* EOP (bit5) and the error bits (PGERR bit2, WRPRTERR bit4) are
		 * write-1-clear; stm32f1_flash_clear_eop() relies on this. BSY is RO. */
		g_flash_sr &= ~(value & (FPEC_SR_EOP | 0x14u));
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------------ *
 *  Unified memory access. Returns the read value; on write, applies value.
 * ------------------------------------------------------------------------ */
static uint32_t mem_access(uint32_t addr, bool write, uint32_t value, emu_access_e acc)
{
	/* RAM: SRAM */
	if (addr >= SRAM_BASE && addr < SRAM_BASE + EMU_SRAM_SIZE) {
		uint8_t *p = &g_sram[addr - SRAM_BASE];
		if (!write) {
			if (acc == EMU_ACCESS_BYTE) return p[0];
			if (acc == EMU_ACCESS_HALF) return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
			return rd32(p);
		}
		if (acc == EMU_ACCESS_BYTE) p[0] = (uint8_t)value;
		else if (acc == EMU_ACCESS_HALF) { p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8); }
		else wr32(p, value);
		return 0;
	}
	/* Flash: reads always allowed; writes require PG set (FPEC programming).
	 * Flash bits can only be cleared, mirroring real NOR program behaviour. */
	if (addr >= FLASH_BASE && addr < FLASH_BASE + EMU_FLASH_SIZE) {
		uint8_t *p = &g_flash[addr - FLASH_BASE];
		if (!write) {
			if (acc == EMU_ACCESS_BYTE) return p[0];
			if (acc == EMU_ACCESS_HALF) return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
			return rd32(p);
		}
		if ((g_flash_cr & FPEC_CR_PG) && !(g_flash_cr & FPEC_CR_LOCK)) {
			/* program: AND into existing (1->0 only), STM32F1 is halfword */
			p[0] &= (uint8_t)value;
			p[1] &= (uint8_t)(value >> 8);
			if (acc == EMU_ACCESS_WORD) {
				p[2] &= (uint8_t)(value >> 16);
				p[3] &= (uint8_t)(value >> 24);
			}
			g_flash_sr |= FPEC_SR_EOP;
		}
		return 0;
	}
	/* Cortex-M SCS / debug */
	if (addr >= SCS_BASE && addr < SCS_BASE + 0x1000u) {
		if (write) { scs_write(addr, value); return 0; }
		return scs_read(addr);
	}
	/* ROM table + its component IDs */
	if (addr >= ROM_BASE && addr < ROM_BASE + 0x1000u)
		return write ? 0 : rom_table_read(addr);
	/* DWT / FPB / ITM component ID stubs so the ROM walk enumerates them */
	if (addr >= 0xe0001000u && addr < 0xe0001000u + 0x1000u)
		return write ? 0 : component_id_read(0xe0001000u, addr, 0x002); /* DWT */
	if (addr >= 0xe0002000u && addr < 0xe0002000u + 0x1000u)
		return write ? 0 : component_id_read(0xe0002000u, addr, 0x003); /* FPB */
	if (addr >= 0xe0000000u && addr < 0xe0000000u + 0x1000u)
		return write ? 0 : component_id_read(0xe0000000u, addr, 0x001); /* ITM */
	/* DBGMCU identity */
	if (addr == DBGMCU_IDCODE)
		return write ? 0 : EMU_DBGMCU_IDCODE;
	/* FPEC */
	if (addr >= FPEC_BASE && addr <= FPEC_WRPR) {
		if (write) { fpec_write(addr, value); return 0; }
		return fpec_read(addr);
	}
	/* Unmodelled peripheral: read as 0, ignore writes. */
	return 0;
}

/* ------------------------------------------------------------------------ *
 *  AP register file (MEM-AP)
 * ------------------------------------------------------------------------ */
static uint32_t ap_reg_read(uint16_t addr)
{
	switch (addr & 0xfcu) {
	case 0x00: return g_ap_csw;
	case 0x04: return g_ap_tar;
	case 0x0c: { /* DRW: read memory at TAR */
		uint32_t v = mem_access(g_ap_tar, false, 0, (emu_access_e)(g_ap_csw & 3u));
		if (((g_ap_csw >> 4) & 3u) == 1u) /* addr-inc single */
			g_ap_tar += (1u << (g_ap_csw & 3u));
		return v;
	}
	case 0xf4: return EMU_AP_CFG;   /* CFG  */
	case 0xf8: return EMU_AP_BASE;  /* BASE */
	case 0xfc: return EMU_AP_IDR;   /* IDR  */
	default: return 0;
	}
}

static void ap_reg_write(uint16_t addr, uint32_t value)
{
	switch (addr & 0xfcu) {
	case 0x00: g_ap_csw = value; break;
	case 0x04: g_ap_tar = value; break;
	case 0x0c: /* DRW: write memory at TAR */
		mem_access(g_ap_tar, true, value, (emu_access_e)(g_ap_csw & 3u));
		if (((g_ap_csw >> 4) & 3u) == 1u)
			g_ap_tar += (1u << (g_ap_csw & 3u));
		break;
	default: break;
	}
}

/* ======================================================================== *
 *  Public transaction entry points
 * ======================================================================== */
uint32_t emu_dp_read(uint16_t addr)
{
	switch (addr & 0x0cu) {
	case 0x0: return EMU_DPIDR;                       /* DPIDR              */
	case 0x4: return g_dp_ctrlstat;                   /* CTRL/STAT          */
	case 0x8: return g_dp_select;                     /* (read rarely used) */
	case 0xc: return g_rdbuff;                        /* RDBUFF             */
	default: return 0;
	}
}

uint32_t emu_dp_low_access(uint8_t rnw, uint16_t addr, uint32_t value)
{
	if (rnw)
		return emu_dp_read(addr);
	switch (addr & 0x0cu) {
	case 0x0: /* ABORT */
		emu_dp_abort(value);
		break;
	case 0x4: /* CTRL/STAT: mirror power/reset requests into their acks */
		g_dp_ctrlstat = value & ~CTRL_STICKY_MASK;
		if (value & CTRL_CDBGPWRUPREQ) g_dp_ctrlstat |= CTRL_CDBGPWRUPACK;
		if (value & CTRL_CSYSPWRUPREQ) g_dp_ctrlstat |= CTRL_CSYSPWRUPACK;
		if (value & CTRL_CDBGRSTREQ)   g_dp_ctrlstat |= CTRL_CDBGRSTACK;
		break;
	case 0x8: /* SELECT */
		g_dp_select = value;
		break;
	default:
		break;
	}
	return 0;
}

void emu_dp_abort(uint32_t abort)
{
	(void)abort;
	g_dp_ctrlstat &= ~CTRL_STICKY_MASK; /* clear sticky errors */
}

uint32_t emu_dp_error(void)
{
	uint32_t err = g_dp_ctrlstat & CTRL_STICKY_MASK;
	g_dp_ctrlstat &= ~CTRL_STICKY_MASK;
	return err;
}

uint32_t emu_ap_read(uint8_t apsel, uint16_t addr)
{
	if (apsel != 0) return 0; /* only AP0 exists */
	g_rdbuff = ap_reg_read(addr);
	return g_rdbuff;
}

void emu_ap_write(uint8_t apsel, uint16_t addr, uint32_t value)
{
	if (apsel != 0) return;
	ap_reg_write(addr, value);
}

void emu_mem_read(uint32_t addr, void *dest, size_t len)
{
	uint8_t *d = (uint8_t *)dest;
	/* word-aligned fast path, byte tail */
	while (len >= 4u) {
		wr32(d, mem_access(addr, false, 0, EMU_ACCESS_WORD));
		addr += 4u; d += 4u; len -= 4u;
	}
	while (len--) {
		*d++ = (uint8_t)mem_access(addr, false, 0, EMU_ACCESS_BYTE);
		addr += 1u;
	}
}

void emu_mem_write(uint32_t addr, const void *src, size_t len, emu_access_e access)
{
	const uint8_t *s = (const uint8_t *)src;
	if (access == EMU_ACCESS_HALF) {
		while (len >= 2u) {
			mem_access(addr, true, (uint32_t)s[0] | ((uint32_t)s[1] << 8), EMU_ACCESS_HALF);
			addr += 2u; s += 2u; len -= 2u;
		}
	} else if (access == EMU_ACCESS_WORD) {
		while (len >= 4u) {
			mem_access(addr, true, rd32(s), EMU_ACCESS_WORD);
			addr += 4u; s += 4u; len -= 4u;
		}
	}
	while (len--) {
		mem_access(addr, true, *s++, EMU_ACCESS_BYTE);
		addr += 1u;
	}
}
