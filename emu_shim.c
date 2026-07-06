/*
 * emu_shim.c - turns ADIv5 DP/AP transactions into calls on the emulated
 * STM32F103 model (emu_target.c). Implements posted-read semantics identical
 * to firmware_swdp_read()/firmware_swdp_low_access() so BMP's memory access
 * loops (advi5_mem_read_bytes) unpack the right data.
 *
 * Transport state (all single-DP; only one emulated target at a time):
 *  - g_posted : last value latched by a posted AP read (returned via RDBUFF)
 *  - g_tar    : AHB-AP Transfer Address Register
 *  - g_csw    : AHB-AP Control/Status Word (drives auto-increment + size)
 *  - g_ctrlstat: DP CTRL/STAT (ack bits mirror the request bits written)
 *  - g_select : DP SELECT (APSEL + APBANKSEL)
 */
#include "general.h"
#include "adiv5.h"
#include "emu_shim.h"
#include "emu_target.h"

static uint32_t g_posted;
static uint32_t g_tar;
static uint32_t g_csw;
static uint32_t g_ctrlstat;
static uint32_t g_select;

/* Decode the current AHB-AP access size from CSW (bytes). */
static uint32_t csw_access_size(void)
{
	switch (g_csw & ADIV5_AP_CSW_SIZE_MASK) {
	case ADIV5_AP_CSW_SIZE_BYTE:
		return 1U;
	case ADIV5_AP_CSW_SIZE_HALFWORD:
		return 2U;
	default:
		return 4U;
	}
}

static bool csw_autoinc(void)
{
	return (g_csw & ADIV5_AP_CSW_ADDRINC_MASK) == ADIV5_AP_CSW_ADDRINC_SINGLE;
}

/* CTRL/STAT read: reflect requested power-up as acknowledged. */
static uint32_t ctrlstat_value(void)
{
	uint32_t v = g_ctrlstat;
	if (g_ctrlstat & ADIV5_DP_CTRLSTAT_CSYSPWRUPREQ)
		v |= ADIV5_DP_CTRLSTAT_CSYSPWRUPACK;
	if (g_ctrlstat & ADIV5_DP_CTRLSTAT_CDBGPWRUPREQ)
		v |= ADIV5_DP_CTRLSTAT_CDBGPWRUPACK;
	return v;
}

/* ---- AP register access (posted read model) ----------------------------- */
static uint32_t ap_reg_read(uint16_t reg)
{
	/* Return the previously posted value, then post the new one. */
	uint32_t ret = g_posted;

	/* Only apsel 0 is populated. Every other AP reads as absent so the
	 * DP scan (adiv5.c:952) stops after 8 invalid APs instead of
	 * registering a target at all 256 apsel values. adiv5_new_ap treats
	 * BASE==0xffffffff or IDR==0 as "no AP here". */
	if ((uint8_t)(g_select >> 24U) != 0U) {
		switch (reg) {
		case 0xF8U: /* BASE - not present */
			g_posted = 0xFFFFFFFFU;
			break;
		default: /* IDR invalid, everything else zero */
			g_posted = 0U;
			break;
		}
		return ret;
	}

	switch (reg) {
	case 0x00U: /* CSW  */
		g_posted = g_csw | ADIV5_AP_CSW_DBGSWENABLE;
		break;
	case 0x04U: /* TAR  */
		g_posted = g_tar;
		break;
	case 0x0CU: /* DRW  */
	{
		const uint32_t size = csw_access_size();
		const uint32_t shift = 8U * (g_tar & (4U - size));
		g_posted = emu_target_load(g_tar, size) << shift;
		if (csw_autoinc())
			g_tar += size;
		break;
	}
	case 0xF4U: /* CFG  */
		g_posted = EMU_AP_CFG;
		break;
	case 0xF8U: /* BASE */
		g_posted = EMU_AP_BASE;
		break;
	case 0xFCU: /* IDR  */
		g_posted = EMU_AP_IDR;
		break;
	default:
		g_posted = 0U;
		break;
	}
	return ret;
}

static void ap_reg_write(uint16_t reg, uint32_t value)
{
	switch (reg) {
	case 0x00U: /* CSW */
		g_csw = value;
		break;
	case 0x04U: /* TAR */
		g_tar = value;
		break;
	case 0x0CU: /* DRW */
	{
		const uint32_t size = csw_access_size();
		const uint32_t shift = 8U * (g_tar & (4U - size));
		const uint32_t lane = (value >> shift) & (size >= 4U ? 0xFFFFFFFFU : ((1U << (size * 8U)) - 1U));
		emu_target_store(g_tar, lane, size);
		if (csw_autoinc())
			g_tar += size;
		break;
	}
	default:
		break;
	}
}

/* ---- DP primitives ------------------------------------------------------ */
uint32_t emu_shim_low_access(adiv5_debug_port_s *dp, uint8_t rnw, uint16_t addr, uint32_t value)
{
	(void)dp;
	if (addr & ADIV5_APnDP) {
		const uint16_t reg = addr & 0xFCU;
		if (rnw == ADIV5_LOW_READ)
			return ap_reg_read(reg);
		ap_reg_write(reg, value);
		return 0U;
	}

	/* DP register access. */
	const uint16_t reg = addr & 0x0CU;
	if (rnw == ADIV5_LOW_READ) {
		switch (reg) {
		case 0x0U: /* DPIDR */
			return EMU_DPIDR;
		case 0x4U: /* CTRL/STAT (bank 0; DPv1 never selects bank 2) */
			return ctrlstat_value();
		case 0xCU: /* RDBUFF - completes a posted AP read */
			return g_posted;
		default:
			return 0U;
		}
	}
	/* DP write */
	switch (reg) {
	case 0x0U: /* ABORT */
		dp->fault = 0U;
		break;
	case 0x4U: /* CTRL/STAT */
		g_ctrlstat = value;
		break;
	case 0x8U: /* SELECT */
		g_select = value;
		break;
	default:
		break;
	}
	return 0U;
}

/* Mirror of firmware_swdp_read(): AP reads are posted, DP reads are direct. */
uint32_t emu_shim_dp_read(adiv5_debug_port_s *dp, uint16_t addr)
{
	if (addr & ADIV5_APnDP) {
		emu_shim_low_access(dp, ADIV5_LOW_READ, addr, 0U);
		return emu_shim_low_access(dp, ADIV5_LOW_READ, ADIV5_DP_RDBUFF, 0U);
	}
	return emu_shim_low_access(dp, ADIV5_LOW_READ, addr, 0U);
}

uint32_t emu_shim_error(adiv5_debug_port_s *dp, bool protocol_recovery)
{
	(void)protocol_recovery;
	dp->fault = 0U;
	return 0U; /* emulated target never faults */
}

void emu_shim_abort(adiv5_debug_port_s *dp, uint32_t abort)
{
	(void)abort;
	dp->fault = 0U;
}

/* ---- scan entry --------------------------------------------------------- */
bool emu_scan(void)
{
	g_posted = 0U;
	g_tar = 0U;
	g_csw = 0U;
	g_ctrlstat = 0U;
	g_select = 0U;
	emu_target_reset();

	adiv5_debug_port_s *dp = calloc(1, sizeof(*dp));
	if (!dp) {
		DEBUG_ERROR("emu_scan: calloc failed\n");
		return false;
	}

	dp->dp_read = emu_shim_dp_read;
	dp->low_access = emu_shim_low_access;
	dp->error = emu_shim_error;
	dp->abort = emu_shim_abort;
	/* designer_code / partno left 0: adiv5_dp_init reads DPIDR from us
	 * (0 != JEP106_MANUFACTURER_ARM satisfies the guard at adiv5.c:831). */

	adiv5_dp_init(dp);
	/* adiv5_dp_init frees dp on failure; on success it is refcounted by the
	 * discovered AP(s) and the target is registered. */
	return true;
}