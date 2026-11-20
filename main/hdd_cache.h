/*
 * Fast, cached disk image access for tiny386's ide.c, which uses it for its
 * hard disk and CD images when built with IDE_ENABLE_HDD_CACHE (see
 * board_m5tab5.h and block_device_init_hddc() in tiny386/ide.c).
 *
 * Through stdio, disk images on this board are slow:
 *   - every IDE command (at most 2 KB) is a separate SD card transaction,
 *   - the guest's buffer is not DMA-aligned, so the SD driver bounces it
 *     through a temporary buffer one 512-byte sector at a time,
 *   - FatFs follows the file's cluster chain from the start on every
 *     backward seek, which for a large image means hundreds of FAT reads.
 *
 * hdd_open() maps an image's clusters to card sectors once (FatFs fast-seek
 * link map). After that, reads and writes go straight to the card with DMA,
 * and reads go through an LRU block cache in PSRAM that also reads ahead.
 * Writes go through to the card before they return (the cache is never
 * dirty), so pulling the power loses nothing ide.c reported as written. The
 * image cannot grow, so its FAT entries never change.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct HddImage HddImage;

/* An image on the SD card (/sdcard/...), or NULL: not there, not a whole
   number of clusters on a 512-byte-sector card, or no free slot. The caller
   then falls back to stdio. */
HddImage *hdd_open(const char *path, bool writable);
void hdd_close(HddImage *h);

/* whole 512-byte sectors in the image */
int64_t hdd_sector_count(HddImage *h);

/* n sectors from sector on; 0, or -1 on an error or out of range */
int hdd_read(HddImage *h, uint64_t sector, uint8_t *buf, int n);
int hdd_write(HddImage *h, uint64_t sector, const uint8_t *buf, int n);
