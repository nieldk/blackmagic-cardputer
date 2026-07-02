#include "bmp_standalone_load.h"
#include <string.h>
#include <stdarg.h>

#define EI_NIDENT   16
#define ELFCLASS32  1
#define ELFDATA2LSB 1
#define EM_ARM      40
#define PT_LOAD     1

typedef struct __attribute__((packed)) {
	uint8_t  e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint32_t e_entry;
	uint32_t e_phoff;
	uint32_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
} elf32_ehdr_s;

typedef struct __attribute__((packed)) {
	uint32_t p_type;
	uint32_t p_offset;
	uint32_t p_vaddr;
	uint32_t p_paddr;
	uint32_t p_filesz;
	uint32_t p_memsz;
	uint32_t p_flags;
	uint32_t p_align;
} elf32_phdr_s;

/* No PSRAM: keep the streaming buffer static, off the task stack and heap.
   The loader is modal (one flash op at a time), so this is safe. */
#define CHUNK 2048
static uint8_t chunk_buf[CHUNK];

#define FAIL(m) do { if (err) *err = (m); return false; } while (0)

static void slog(const flash_sink_s *s, const char *fmt, ...)
{
	if (!s->log)
		return;
	char line[96];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	s->log(s->ctx, line);
}

static bool read_at(FILE *f, long off, void *dst, size_t n)
{
	return fseek(f, off, SEEK_SET) == 0 && fread(dst, 1, n, f) == n;
}

/* True if this program header is a loadable segment we should flash. */
static bool seg_flashable(const flash_sink_s *sink, const elf32_phdr_s *ph)
{
	if (ph->p_type != PT_LOAD || ph->p_filesz == 0)
		return false;
	if (sink->in_flash && !sink->in_flash(ph->p_paddr, ph->p_filesz, sink->ctx))
		return false;
	return true;
}

static bool verify_range(FILE *f, long file_off, uint32_t addr, uint32_t len,
                         const flash_sink_s *sink, const char **err)
{
	uint8_t want[256], got[256];
	if (fseek(f, file_off, SEEK_SET) != 0) {
		FAIL("seek (verify)");
	}
	while (len) {
		size_t n = len < sizeof want ? len : sizeof want;
		if (fread(want, 1, n, f) != n) {
			FAIL("short read (verify)");
		}
		if (!sink->read(addr, got, n, sink->ctx)) {
			FAIL("readback failed");
		}
		if (memcmp(want, got, n) != 0) {
			FAIL("verify mismatch");
		}
		addr += n;
		len  -= n;
	}
	return true;
}

bool bmp_load_elf(FILE *f, const flash_sink_s *sink, load_result_s *out, const char **err)
{
	elf32_ehdr_s eh;
	if (!read_at(f, 0, &eh, sizeof eh))                 { FAIL("short read (ehdr)"); }
	if (!(eh.e_ident[0] == 0x7f && eh.e_ident[1] == 'E' &&
	      eh.e_ident[2] == 'L'  && eh.e_ident[3] == 'F')) { FAIL("not an ELF"); }
	if (eh.e_ident[4] != ELFCLASS32)                    { FAIL("not ELF32"); }
	if (eh.e_ident[5] != ELFDATA2LSB)                   { FAIL("not little-endian"); }
	if (eh.e_machine  != EM_ARM)                        { FAIL("not EM_ARM"); }
	if (eh.e_phnum == 0 || eh.e_phentsize < sizeof(elf32_phdr_s)) {
		FAIL("no/bad program headers");
	}

	/* Pass 1: union span of flashable segments, so we erase once. NOTE: this
	   over-erases any gap between segments. For a single firmware image that
	   is what you want; if you keep a config sector wedged between app regions,
	   switch to per-segment erase with an erased-sector set. */
	uint32_t lo = 0xffffffffu, hi = 0;
	unsigned n_load = 0;
	for (unsigned i = 0; i < eh.e_phnum; i++) {
		elf32_phdr_s ph;
		if (!read_at(f, eh.e_phoff + (long)i * eh.e_phentsize, &ph, sizeof ph)) {
			FAIL("short read (phdr)");
		}
		if (!seg_flashable(sink, &ph))
			continue;
		if (ph.p_paddr < lo)                 lo = ph.p_paddr;
		if (ph.p_paddr + ph.p_filesz > hi)   hi = ph.p_paddr + ph.p_filesz;
		n_load++;
	}
	if (n_load == 0) { FAIL("no flashable PT_LOAD"); }

	slog(sink, "erase 0x%08lx +%lu", (unsigned long)lo, (unsigned long)(hi - lo));
	if (!sink->erase(lo, hi - lo, sink->ctx)) { FAIL("erase failed"); }

	/* Pass 2: program each segment from its file offset to its paddr (LMA). */
	uint32_t total = 0;
	unsigned done = 0;
	for (unsigned i = 0; i < eh.e_phnum; i++) {
		elf32_phdr_s ph;
		if (!read_at(f, eh.e_phoff + (long)i * eh.e_phentsize, &ph, sizeof ph)) {
			FAIL("short read (phdr)");
		}
		if (!seg_flashable(sink, &ph))
			continue;

		if (fseek(f, ph.p_offset, SEEK_SET) != 0) { FAIL("seek (segment)"); }
		uint32_t addr = ph.p_paddr, left = ph.p_filesz;
		while (left) {
			size_t want = left < CHUNK ? left : CHUNK;
			if (fread(chunk_buf, 1, want, f) != want) { FAIL("short read (segment)"); }
			if (!sink->write(addr, chunk_buf, want, sink->ctx)) { FAIL("write failed"); }
			addr  += want;
			left  -= want;
			total += want;
		}
		done++;
		slog(sink, "seg %u/%u 0x%08lx +%lu ok", done, n_load,
		     (unsigned long)ph.p_paddr, (unsigned long)ph.p_filesz);
	}

	if (!sink->complete(sink->ctx)) { FAIL("flash complete failed"); }

	if (sink->read) {
		for (unsigned i = 0; i < eh.e_phnum; i++) {
			elf32_phdr_s ph;
			if (!read_at(f, eh.e_phoff + (long)i * eh.e_phentsize, &ph, sizeof ph)) {
				FAIL("short read (phdr/verify)");
			}
			if (!seg_flashable(sink, &ph))
				continue;
			if (!verify_range(f, ph.p_offset, ph.p_paddr, ph.p_filesz, sink, err))
				return false;
		}
		slog(sink, "verify ok");
	}

	if (out) {
		out->segments = done;
		out->bytes    = total;
		out->entry    = eh.e_entry;
	}
	return true;
}

bool bmp_load_bin(FILE *f, uint32_t base, const flash_sink_s *sink, load_result_s *out, const char **err)
{
	if (fseek(f, 0, SEEK_END) != 0) { FAIL("seek (size)"); }
	long sz = ftell(f);
	if (sz <= 0)                    { FAIL("empty/bad file"); }
	if (sink->in_flash && !sink->in_flash(base, (size_t)sz, sink->ctx)) {
		FAIL("target range not flash");
	}

	slog(sink, "erase 0x%08lx +%lu", (unsigned long)base, (unsigned long)sz);
	if (!sink->erase(base, (size_t)sz, sink->ctx)) { FAIL("erase failed"); }

	if (fseek(f, 0, SEEK_SET) != 0) { FAIL("seek (data)"); }
	uint32_t addr = base, total = 0, left = (uint32_t)sz;
	while (left) {
		size_t want = left < CHUNK ? left : CHUNK;
		if (fread(chunk_buf, 1, want, f) != want) { FAIL("short read (data)"); }
		if (!sink->write(addr, chunk_buf, want, sink->ctx)) { FAIL("write failed"); }
		addr  += want;
		left  -= want;
		total += want;
	}

	if (!sink->complete(sink->ctx)) { FAIL("flash complete failed"); }

	if (sink->read && !verify_range(f, 0, base, (uint32_t)sz, sink, err))
		return false;
	if (sink->read)
		slog(sink, "verify ok");

	if (out) {
		out->segments = 1;
		out->bytes    = total;
		out->entry    = base;
	}
	return true;
}
