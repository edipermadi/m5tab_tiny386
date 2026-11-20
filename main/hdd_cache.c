/*
 * Raw, cached access to disk images on the SD card. See hdd_cache.h.
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h" /* esp_cache_get_alignment(), as the SD driver uses */
#include "sdmmc_cmd.h"
#include "ff.h"
#include "diskio_sdmmc.h"

#include "hdd_cache.h"

#if !CONFIG_FATFS_USE_FASTSEEK
#error "hdd_cache needs CONFIG_FATFS_USE_FASTSEEK=y (FatFs link map)"
#endif

static const char *TAG = "hdd_cache";

#define SECT 512
#define MOUNT_POINT "/sdcard/"
#define MAX_FILES 4 /* images open at once */

#define BLOCK_SECTS (CONFIG_TAB5_HDD_CACHE_BLOCK_KB * 1024 / SECT)
#define BLOCK_BYTES (BLOCK_SECTS * SECT)
#define STAGE_SECTS 128 /* one gathered IDE write (64 KB) */

extern void *rawsd; /* storage_tab5.c */

/* a run of the file's sectors that is contiguous on the card */
typedef struct {
    uint32_t file_sect;
    uint32_t nsect;
    uint64_t lba;
} Extent;

typedef struct Block Block;
struct Block {
    HddImage *owner;
    uint32_t blk;           /* block number within the file */
    uint32_t nsect;         /* valid sectors (the last block may be short) */
    Block *hnext;           /* hash chain */
    Block *prev, *next;     /* LRU list, most recent at the head */
    uint8_t *data;
};

struct HddImage {
    bool used;
    bool writable;
    sdmmc_card_t *card;
    Extent *ext;
    int next;
    int last_ext;           /* last extent hit: accesses are mostly local */
    uint32_t nsect;         /* whole sectors in the image */
};

static HddImage images[MAX_FILES];

/* cache, shared by all images */
static Block *blocks;
static int nblocks;
static Block **hash;
static int hash_mask;
static Block lru; /* sentinel */
static uint8_t *stage; /* DMA-aligned bounce for the callers' buffers */
static size_t dma_align;

static uint32_t stat_hits, stat_misses;

/* ------------------------------------------------------------ setup */

static void *dma_alloc(size_t size)
{
    size = (size + dma_align - 1) / dma_align * dma_align;
    return heap_caps_aligned_calloc(dma_align, 1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
}

static bool cache_init(void)
{
    if (stage) {
        return true;
    }
    if (esp_cache_get_alignment(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, &dma_align) != ESP_OK ||
        dma_align < 4) {
        dma_align = 128;
    }
    stage = dma_alloc(STAGE_SECTS * SECT);
    if (!stage) {
        return false;
    }

    /* as many blocks as fit, up to the configured size */
    int want = (int)((size_t)CONFIG_TAB5_HDD_CACHE_MB * 1024 * 1024 / BLOCK_BYTES);
    blocks = calloc(want, sizeof(Block));
    if (!blocks) {
        want = 0;
    }
    for (nblocks = 0; nblocks < want; nblocks++) {
        blocks[nblocks].data = dma_alloc(BLOCK_BYTES);
        if (!blocks[nblocks].data) {
            break;
        }
    }
    int hsize = 1;
    while (hsize < nblocks * 2) {
        hsize <<= 1;
    }
    hash = calloc(hsize, sizeof(Block *));
    hash_mask = hsize - 1;
    if (!hash) {
        nblocks = 0;
    }

    lru.prev = lru.next = &lru;
    for (int i = 0; i < nblocks; i++) {
        Block *b = &blocks[i];
        b->prev = lru.prev;
        b->next = &lru;
        lru.prev->next = b;
        lru.prev = b;
    }
    ESP_LOGI(TAG, "block cache: %d x %d KB in PSRAM", nblocks, BLOCK_BYTES / 1024);
    return true;
}

/* Map the clusters of the image to card sectors. */
static bool map_file(HddImage *h, const char *path)
{
    sdmmc_card_t *card = rawsd;
    BYTE pdrv = ff_diskio_get_pdrv_card(card);
    if (pdrv == 0xFF) {
        return false;
    }
    char fpath[256];
    snprintf(fpath, sizeof(fpath), "%u:/%s", pdrv, path + strlen(MOUNT_POINT));

    FIL *fil = calloc(1, sizeof(FIL));
    if (!fil) {
        return false;
    }
    bool ok = false;
    DWORD *tbl = NULL;
    if (f_open(fil, fpath, FA_READ) != FR_OK) {
        free(fil);
        return false;
    }
    FATFS *fs = fil->obj.fs;
#if FF_MAX_SS != FF_MIN_SS
    if (fs->ssize != SECT) {
        goto out;
    }
#endif
    if (card->csd.sector_size != SECT) {
        goto out;
    }

    /* link map: tbl[0] = size; then (clusters, first cluster) pairs, 0-ended */
    DWORD tsize = 64;
    for (;;) {
        free(tbl);
        tbl = malloc(tsize * sizeof(DWORD));
        if (!tbl) {
            goto out;
        }
        tbl[0] = tsize;
        fil->cltbl = tbl;
        FRESULT r = f_lseek(fil, CREATE_LINKMAP);
        if (r == FR_OK) {
            break;
        }
        if (r != FR_NOT_ENOUGH_CORE || tbl[0] <= tsize) {
            goto out;
        }
        tsize = tbl[0];
    }

    int n = 0;
    for (DWORD *p = tbl + 1; *p; p += 2) {
        n++;
    }
    h->ext = malloc((n ? n : 1) * sizeof(Extent));
    if (!h->ext) {
        goto out;
    }
    uint32_t fsect = 0;
    int i = 0;
    for (DWORD *p = tbl + 1; *p; p += 2, i++) {
        h->ext[i].file_sect = fsect;
        h->ext[i].nsect = p[0] * fs->csize;
        h->ext[i].lba = fs->database + (uint64_t)fs->csize * (p[1] - 2);
        fsect += h->ext[i].nsect;
    }
    h->next = n;
    FSIZE_t size = f_size(fil);
    h->nsect = (uint32_t)(size / SECT);
    if (h->nsect > fsect) { /* the clusters must cover the file */
        free(h->ext);
        h->ext = NULL;
        goto out;
    }
    h->card = card;
    ok = true;
    ESP_LOGI(TAG, "%s: %llu MB in %d fragment%s", path,
             (unsigned long long)(size >> 20), n, n == 1 ? "" : "s");
out:
    fil->cltbl = NULL;
    f_close(fil);
    free(fil);
    free(tbl);
    return ok;
}

/* ------------------------------------------------------------ raw I/O */

/* sectors [s, s + n) of the file <-> buf (DMA-aligned) */
static bool raw_io(HddImage *h, uint32_t s, uint32_t n, uint8_t *buf, bool write)
{
    while (n) {
        /* find the extent of sector s, starting from the last one used */
        int i = h->last_ext;
        if (s < h->ext[i].file_sect || s >= h->ext[i].file_sect + h->ext[i].nsect) {
            int lo = 0, hi = h->next - 1;
            while (lo < hi) {
                int mid = (lo + hi + 1) / 2;
                if (h->ext[mid].file_sect <= s) {
                    lo = mid;
                } else {
                    hi = mid - 1;
                }
            }
            i = h->last_ext = lo;
        }
        const Extent *e = &h->ext[i];
        uint32_t off = s - e->file_sect;
        uint32_t k = e->nsect - off;
        if (k > n) {
            k = n;
        }
        esp_err_t r = write ? sdmmc_write_sectors(h->card, buf, e->lba + off, k)
                            : sdmmc_read_sectors(h->card, buf, e->lba + off, k);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "%s sector %lu+%lu: %s", write ? "write" : "read",
                     (unsigned long)s, (unsigned long)k, esp_err_to_name(r));
            return false;
        }
        s += k;
        n -= k;
        buf += k * SECT;
    }
    return true;
}

/* ------------------------------------------------------------ cache */

static Block *lookup(HddImage *h, uint32_t blk)
{
    for (Block *b = hash[blk & hash_mask]; b; b = b->hnext) {
        if (b->blk == blk && b->owner == h) {
            return b;
        }
    }
    return NULL;
}

static void touch(Block *b)
{
    b->prev->next = b->next;
    b->next->prev = b->prev;
    b->next = lru.next;
    b->prev = &lru;
    lru.next->prev = b;
    lru.next = b;
}

static void unhash(Block *b)
{
    if (!b->owner) {
        return;
    }
    Block **pp = &hash[b->blk & hash_mask];
    while (*pp != b) {
        pp = &(*pp)->hnext;
    }
    *pp = b->hnext;
    b->owner = NULL;
}

static Block *get_block(HddImage *h, uint32_t blk)
{
    Block *b = lookup(h, blk);
    if (b) {
        stat_hits++;
        touch(b);
        return b;
    }
    stat_misses++;
    b = lru.prev; /* least recently used */
    unhash(b);
    uint32_t s = blk * BLOCK_SECTS;
    uint32_t n = h->nsect - s < BLOCK_SECTS ? h->nsect - s : BLOCK_SECTS;
    if (!raw_io(h, s, n, b->data, false)) {
        return NULL;
    }
    b->owner = h;
    b->blk = blk;
    b->nsect = n;
    b->hnext = hash[blk & hash_mask];
    hash[blk & hash_mask] = b;
    touch(b);
    if (((stat_hits + stat_misses) & 0xffff) == 0) {
        ESP_LOGI(TAG, "hit rate %lu%%",
                 (unsigned long)((uint64_t)stat_hits * 100 / (stat_hits + stat_misses)));
    }
    return b;
}

/* ------------------------------------------------------------ API */

HddImage *hdd_open(const char *path, bool writable)
{
    if (!rawsd || strncmp(path, MOUNT_POINT, strlen(MOUNT_POINT)) != 0 || !cache_init()) {
        return NULL;
    }
    HddImage *h = NULL;
    for (int i = 0; i < MAX_FILES; i++) {
        if (!images[i].used) {
            h = &images[i];
            break;
        }
    }
    if (!h) {
        return NULL;
    }
    memset(h, 0, sizeof(*h));
    if (!map_file(h, path)) {
        ESP_LOGW(TAG, "%s: no raw access, using stdio", path);
        return NULL;
    }
    h->writable = writable;
    h->used = true;
    return h;
}

void hdd_close(HddImage *h)
{
    for (int i = 0; i < nblocks; i++) { /* drop its blocks */
        if (blocks[i].owner == h) {
            unhash(&blocks[i]);
        }
    }
    free(h->ext);
    h->ext = NULL;
    h->used = false;
}

int64_t hdd_sector_count(HddImage *h)
{
    return h->nsect;
}

int hdd_read(HddImage *h, uint64_t sector, uint8_t *buf, int n)
{
    if (n < 0 || sector + n > h->nsect) {
        return -1;
    }
    uint32_t s = sector;
    while (n) {
        int k;
        if (nblocks) {
            Block *b = get_block(h, s / BLOCK_SECTS);
            if (!b) {
                return -1;
            }
            uint32_t boff = s - b->blk * BLOCK_SECTS;
            k = b->nsect - boff;
            if (k > n) {
                k = n;
            }
            memcpy(buf, b->data + boff * SECT, k * SECT);
        } else { /* no cache: through the staging buffer */
            k = n < STAGE_SECTS ? n : STAGE_SECTS;
            if (!raw_io(h, s, k, stage, false)) {
                return -1;
            }
            memcpy(buf, stage, k * SECT);
        }
        s += k;
        buf += k * SECT;
        n -= k;
    }
    return 0;
}

/* write-through: card first, then the cached copies */
int hdd_write(HddImage *h, uint64_t sector, const uint8_t *buf, int n)
{
    if (!h->writable || n < 0 || sector + n > h->nsect) {
        return -1;
    }
    uint32_t s = sector;
    while (n) {
        int k = n < STAGE_SECTS ? n : STAGE_SECTS;
        memcpy(stage, buf, k * SECT);
        bool ok = raw_io(h, s, k, stage, true);
        for (uint32_t blk = s / BLOCK_SECTS; nblocks && blk <= (s + k - 1) / BLOCK_SECTS; blk++) {
            Block *b = lookup(h, blk);
            if (!b) {
                continue;
            }
            if (!ok) { /* the card may hold old or new data now: forget it */
                unhash(b);
                continue;
            }
            uint32_t bstart = blk * BLOCK_SECTS, bend = bstart + b->nsect;
            uint32_t from = s > bstart ? s : bstart;
            uint32_t to = s + k < bend ? s + k : bend;
            if (from < to) {
                memcpy(b->data + (from - bstart) * SECT, buf + (from - s) * SECT,
                       (to - from) * SECT);
            }
        }
        if (!ok) {
            return -1;
        }
        s += k;
        buf += k * SECT;
        n -= k;
    }
    return 0;
}
