/*
 * Fast, cached disk image access for tiny386's ide.c (force-included into
 * ide.c only, see main/CMakeLists.txt; the submodule is left untouched).
 *
 * ide.c reads and writes its disk images with fopen/fseeko/fread/fwrite. On
 * this board that path is slow:
 *   - every IDE command (at most 2 KB) is a separate SD card transaction,
 *   - the guest's buffer is not DMA-aligned, so the SD driver bounces it
 *     through a temporary buffer one 512-byte sector at a time,
 *   - FatFs follows the file's cluster chain from the start on every
 *     backward seek, which for a large image means hundreds of FAT reads.
 *
 * For an image on the SD card, hdd_fopen() maps its clusters to card sectors
 * once (FatFs fast-seek link map). After that, reads and writes go straight to
 * the card with DMA, and reads go through an LRU block cache in PSRAM that
 * also reads ahead. Writes go through to the card before they return (the
 * cache is never dirty), so pulling the power loses nothing ide.c reported as
 * written. The image cannot grow, so its FAT entries never change.
 *
 * Anything else (another path, the map failing) falls back to plain stdio.
 */
#pragma once

#include <stdio.h>
#include <sys/types.h>

FILE *hdd_fopen(const char *path, const char *mode);
int hdd_fclose(FILE *f);
int hdd_fseeko(FILE *f, off_t offset, int whence);
off_t hdd_ftello(FILE *f);
size_t hdd_fread(void *buf, size_t size, size_t n, FILE *f);
size_t hdd_fwrite(const void *buf, size_t size, size_t n, FILE *f);

#ifdef HDD_CACHE_REDIRECT_STDIO
#undef fopen
#undef fclose
#undef fseeko
#undef ftello
#undef fread
#undef fwrite
#define fopen hdd_fopen
#define fclose hdd_fclose
#define fseeko hdd_fseeko
#define ftello hdd_ftello
#define fread hdd_fread
#define fwrite hdd_fwrite
#endif
