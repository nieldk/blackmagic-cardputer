// Standalone on-device debugger/flasher verbs for the Cardputer BMP.
// Dispatched from ui.c's REPL before command_process(). See ui_debug.h.

#include "ui_debug.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "target.h"               // blackmagic-fw target API
#include "gdb_main.h"             // extern target_s *cur_target;
#include "ui.h"                   // ui_capture_write()
#include "target_lock.h"          // target_lock()/target_unlock()
#include "bmp_standalone_load.h"  // bmp_load_elf()/bmp_load_bin()

#define CMD_COPY_SIZE 96
#define MAX_ARGV      6

// --- output helper: one formatted line to scrollback + CDC UART -------------

static void out(const char *fmt, ...)
{
	char b[128];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(b, sizeof b - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	// Terminate every message with a newline so ui_capture_write() flushes it
	// as its own scrollback line instead of packing multiple out() calls into
	// one 40-column row.
	size_t len = ((size_t)n < sizeof b - 1) ? (size_t)n : sizeof b - 2;
	b[len] = '\n';
	b[len + 1] = '\0';
	ui_capture_write(b);
}

// --- target_controller for on-device attach --------------------------------
// Must stay valid for the lifetime of the attach (target->tc points at it),
// so it is static. printf routes target-layer chatter to the screen.

static void ui_ctrl_printf(target_controller_s *tc, const char *fmt, va_list ap)
{
	(void)tc;
	char b[128];
	vsnprintf(b, sizeof b, fmt, ap);
	ui_capture_write(b);
}

static void ui_ctrl_destroy(target_controller_s *tc, target_s *target)
{
	(void)tc;
	(void)target;
	cur_target = NULL;
}

static target_controller_s ui_controller = {
	.destroy_callback = ui_ctrl_destroy,
	.printf = ui_ctrl_printf,
};

// --- flash sink bound to target_flash_* (confirmed from target.h) ----------
// ctx is the attached target_s*. These are raw target calls: the caller holds
// target_lock across the whole load, so they must NOT take it themselves.

static bool sink_erase(uint32_t a, size_t l, void *c)
{
	return target_flash_erase((target_s *)c, a, l);
}

static bool sink_write(uint32_t a, const void *d, size_t l, void *c)
{
	return target_flash_write((target_s *)c, a, d, l);
}

static bool sink_done(void *c)
{
	return target_flash_complete((target_s *)c);
}

static bool sink_read(uint32_t a, void *d, size_t l, void *c)
{
	return target_mem_read((target_s *)c, d, a, l) == 0;
}

static void sink_log(void *c, const char *msg)
{
	(void)c;
	out("%s", msg);
}

// --- verbs -----------------------------------------------------------------

static void v_attach(int argc, char **argv)
{
	size_t n = (argc > 1) ? (size_t)strtoul(argv[1], NULL, 0) : 1;
	target_lock();
	target_s *t = target_attach_n(n, &ui_controller);
	target_unlock();
	if (!t) {
		out("attach %u failed (run swd_scan first?)", (unsigned)n);
		return;
	}
	cur_target = t;
	out("attached %u: %s / %s", (unsigned)n, target_driver_name(t), target_core_name(t));
}

static void v_detach(void)
{
	if (!cur_target) {
		out("no target");
		return;
	}
	target_lock();
	target_detach(cur_target);
	target_unlock();
	cur_target = NULL;
	out("detached");
}

static void v_flash(int argc, char **argv)
{
	if (!cur_target) {
		out("attach first");
		return;
	}
	if (argc < 2) {
		out("usage: flash <path> [hexaddr]");
		return;
	}
	FILE *f = fopen(argv[1], "rb");
	if (!f) {
		out("open %s failed", argv[1]);
		return;
	}

	uint8_t magic[4] = {0};
	size_t got = fread(magic, 1, sizeof magic, f);
	rewind(f);
	bool is_elf = (got == 4 && magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F');

	flash_sink_s sink = {
		.erase = sink_erase,
		.write = sink_write,
		.complete = sink_done,
		.read = sink_read, // verify after write
		.in_flash = NULL,  // let erase/write report non-flash ranges
		.log = sink_log,
		.ctx = cur_target,
	};

	load_result_s r = {0};
	const char *err = NULL;
	bool ok;

	target_lock();
	if (is_elf)
		ok = bmp_load_elf(f, &sink, &r, &err);
	else {
		uint32_t base = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 16) : 0x08000000u;
		ok = bmp_load_bin(f, base, &sink, &r, &err);
	}
	target_unlock();
	fclose(f);

	if (ok)
		out("flash ok: %u seg, %lu bytes, entry 0x%08lx", r.segments, (unsigned long)r.bytes,
		    (unsigned long)r.entry);
	else
		out("flash failed: %s", err ? err : "?");
}

static void v_regs(void)
{
	if (!cur_target) {
		out("attach first");
		return;
	}
	size_t sz = target_regs_size(cur_target);
	static uint8_t rb[512];
	if (sz == 0 || sz > sizeof rb) {
		out("regs size %u unsupported", (unsigned)sz);
		return;
	}
	target_lock();
	target_regs_read(cur_target, rb);
	target_unlock();
	const uint32_t *w = (const uint32_t *)(const void *)rb;
	for (size_t i = 0; i < sz / 4; i++)
		out("r%02u: %08lx", (unsigned)i, (unsigned long)w[i]);
}

static void v_mem(int argc, char **argv)
{
	if (!cur_target) {
		out("attach first");
		return;
	}
	if (argc < 3) {
		out("usage: mem <hexaddr> <len>");
		return;
	}
	uint32_t addr = (uint32_t)strtoul(argv[1], NULL, 16);
	uint32_t len = (uint32_t)strtoul(argv[2], NULL, 0);
	if (len > 256)
		len = 256;
	static uint8_t mb[256];
	target_lock();
	int rc = target_mem_read(cur_target, mb, addr, len);
	target_unlock();
	if (rc) {
		out("read failed @0x%08lx", (unsigned long)addr);
		return;
	}
	for (uint32_t off = 0; off < len; off += 8) {
		char line[48];
		int p = snprintf(line, sizeof line, "%08lx:", (unsigned long)(addr + off));
		for (uint32_t i = 0; i < 8 && off + i < len; i++)
			p += snprintf(line + p, sizeof line - (size_t)p, " %02x", mb[off + i]);
		out("%s", line);
	}
}

static void v_ctl(const char *verb)
{
	if (!cur_target) {
		out("attach first");
		return;
	}
	target_lock();
	if (!strcmp(verb, "reset")) {
		target_reset(cur_target);
		out("reset");
	} else if (!strcmp(verb, "halt")) {
		target_halt_request(cur_target);
		out("halt requested");
	} else if (!strcmp(verb, "run")) {
		target_halt_resume(cur_target, false);
		out("running");
	} else if (!strcmp(verb, "step")) {
		target_halt_resume(cur_target, true);
		out("stepped");
	} else if (!strcmp(verb, "poll")) {
		target_addr_t watch = 0;
		target_halt_reason_e hr = target_halt_poll(cur_target, &watch);
		out("halt reason %d", (int)hr);
	}
	target_unlock();
}

// Printed ahead of the native monitor help. Lines kept under 40 cols so they
// do not wrap on the ST7789 console.
// Curated help, every line kept under 40 columns so nothing wraps on the
// ST7789 console. `help all` falls through to the core's full cmd_help list.
static void v_help(void)
{
	out("== standalone ==");
	out("attach [N]  attach tgt (def 1)");
	out("detach      detach target");
	out("flash <f> [hex]  elf/bin, SD");
	out("regs        dump registers");
	out("mem <a> <n>  hexdump memory");
	out("halt run step poll  run ctrl");
	out("reset       reset core/nRST");
	out("== monitor ==");
	out("swd_scan [id]  scan SWD");
	out("jtag_scan   scan JTAG");
	out("auto_scan   scan all chains");
	out("targets     list targets");
	out("frequency [hz]  set clock");
	out("connect_rst e|d  under-rst");
	out("tdi_low_reset  nRST,TDI low");
	out("halt_timeout [ms]  timeout");
	out("rtt ...     RTT control");
	out("heapinfo ...  semihost");
	out("debug_bmp e|d  dbg vcom2");
	out("version     fw version");
	out("morse       morse error");
	out("help all = full native list");
}

// --- dispatch --------------------------------------------------------------

bool ui_debug_dispatch(const char *line)
{
	char copy[CMD_COPY_SIZE];
	strncpy(copy, line, sizeof copy - 1);
	copy[sizeof copy - 1] = '\0';

	char *argv[MAX_ARGV];
	int argc = 0;
	char *save = NULL;
	for (char *tok = strtok_r(copy, " \t", &save); tok && argc < MAX_ARGV; tok = strtok_r(NULL, " \t", &save))
		argv[argc++] = tok;
	if (argc == 0)
		return false;

	const char *v = argv[0];
	if (!strcmp(v, "help")) {
		if (argc > 1 && !strcmp(argv[1], "all"))
			return false; // let command_process() print the full native list
		v_help();
		return true;
	}
	if (!strcmp(v, "attach"))
		v_attach(argc, argv);
	else if (!strcmp(v, "detach"))
		v_detach();
	else if (!strcmp(v, "flash"))
		v_flash(argc, argv);
	else if (!strcmp(v, "regs"))
		v_regs();
	else if (!strcmp(v, "mem"))
		v_mem(argc, argv);
	else if (!strcmp(v, "reset")) {
		if (!cur_target)
			return false; // no target: let monitor `reset` pulse nRST
		v_ctl("reset");
	} else if (!strcmp(v, "halt") || !strcmp(v, "run") || !strcmp(v, "step") || !strcmp(v, "poll"))
		v_ctl(v);
	else
		return false; // not ours, let command_process() handle it

	return true;
}