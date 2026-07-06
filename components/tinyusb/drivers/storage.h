/*
 * storage.h - single owner of the internal "storage" FAT partition.
 *
 * The wear-levelling partition is mounted exactly once here. USB-MSC does raw
 * block I/O on it (host owns the disk); the on-device `flash` command borrows
 * the filesystem back with storage_acquire()/storage_release(). The two are
 * mutually exclusive so the FAT is never touched from both sides at once.
 */
#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "wear_levelling.h"

/* Mount the wl partition (label "storage"). Call once at boot, before USB. */
bool storage_init(const char *partition_label);

/* Raw block access used by the MSC callbacks. */
wl_handle_t storage_wl(void);
uint32_t storage_sector_size(void);   /* bytes per block presented to host */
uint32_t storage_sector_count(void);

/* Ownership. While the app owns the FS, MSC reports "medium not ready" so the
 * host leaves it alone. storage_acquire() mounts FatFS at base_path (e.g.
 * "/sdcard"); storage_release() unmounts and hands the disk back to the host. */
bool storage_acquire(const char *base_path);
void storage_release(const char *base_path);
bool storage_app_owns(void);          /* true while the app holds the FS */

#endif /* STORAGE_H */
