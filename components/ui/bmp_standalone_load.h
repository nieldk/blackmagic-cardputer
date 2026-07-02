#ifndef BMP_STANDALONE_LOAD_H
#define BMP_STANDALONE_LOAD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/*
 * On-device firmware loader for the standalone Cardputer BMP.
 *
 * Deliberately independent of the Black Magic Probe target API. It walks an
 * ELF or raw binary from a FILE* (SD card via ESP-IDF VFS) and drives flash
 * through the flash_sink_s callbacks below. Bind those callbacks to your
 * tree's target_flash_* in a small adapter, so this file never has to guess
 * your exact signatures.
 *
 * Assumptions: target ELF is little-endian ARM (Cortex-M), host is
 * little-endian (ESP32-S3), so header fields are read straight from disk with
 * no byte swapping.
 */

typedef struct flash_sink {
	/* Erase flash covering [addr, addr+len). target_flash_erase rounds to
	   sectors internally, so partial ranges are fine. Return false on error. */
	bool (*erase)(uint32_t addr, size_t len, void *ctx);

	/* Program len bytes at addr. May be called with arbitrary, unaligned
	   chunk sizes; the BMP flash layer buffers to page granularity. */
	bool (*write)(uint32_t addr, const void *data, size_t len, void *ctx);

	/* Flush any buffered final page. Maps to target_flash_complete. */
	bool (*complete)(void *ctx);

	/* Optional: read back for verify. NULL disables verification. */
	bool (*read)(uint32_t addr, void *data, size_t len, void *ctx);

	/* Optional: true if [addr,addr+len) is programmable flash. NULL means
	   "attempt everything and let erase/write report the error". */
	bool (*in_flash)(uint32_t addr, size_t len, void *ctx);

	/* Optional progress line, routed to the ST7789 scrollback. */
	void (*log)(void *ctx, const char *msg);

	void *ctx;
} flash_sink_s;

typedef struct {
	unsigned segments; /* PT_LOAD segments programmed */
	uint32_t bytes;    /* total bytes written */
	uint32_t entry;    /* ELF entry point, for a subsequent run */
} load_result_s;

/* Both return true on full success. On failure, *err (if non-NULL) points at a
   short static reason string. */
bool bmp_load_elf(FILE *f, const flash_sink_s *sink, load_result_s *out, const char **err);
bool bmp_load_bin(FILE *f, uint32_t base, const flash_sink_s *sink, load_result_s *out, const char **err);

#endif /* BMP_STANDALONE_LOAD_H */
