/*
 * microSD file access over the USB-C serial console (USB Serial/JTAG), for
 * tools/tab5ctl.py: list a directory, pull (download) and push (upload) files,
 * rename them.
 *
 * Commands are lines starting "@fs ", replies too; everything else on the
 * console (logs) is left alone. Replies start with a newline, so one that
 * lands in the middle of a log line still starts a line of its own. Arguments
 * are separated by spaces and may be quoted with "..." (FAT names cannot
 * contain '"'). Paths are relative to the card's root.
 *
 *   @fs ping <token>         -> @fs pong <token>
 *   @fs ls [dir]             -> @fs D <name> | @fs F <size> <name> ..., @fs ok
 *   @fs get <file>           -> @fs size <n>, @fs d <base64> ..., @fs ok <crc32>
 *   @fs put <file> <size>    -> @fs ready; then per "@fs d <base64>" line
 *                               (at most SDFS_PUT_CHUNK bytes) @fs ack <total>;
 *                               "@fs end <crc32>" -> @fs ok
 *   @fs mv <old> <new>       -> @fs ok
 * plus the Wi-Fi and timezone commands of net_tab5.c ("@fs i <text>" lines
 * are information for the user, before the final ok).
 * Any of them can answer "@fs err <message>" instead. crc32 is zlib's, in hex.
 *
 * The driver drops received data its buffer cannot take, so an upload sends
 * one data line and waits for its ack. An upload goes to "<file>.part" and
 * replaces <file> only once its size and CRC match. Disk images the emulator
 * has open cannot be replaced or renamed (sdfs_set_busy()).
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <dirent.h>
#include <inttypes.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

#include "tab5.h"

static const char *TAG = "sdfs";

#ifndef SDFS_ROOT
#define SDFS_ROOT        "/sdcard"
#endif
#define SDFS_PUT_CHUNK   8192            /* bytes per upload line */
#define SDFS_GET_CHUNK   3072            /* bytes per download line (4096 chars) */
#define LINE_MAX_LEN     (16 + SDFS_PUT_CHUNK * 4 / 3 + 16)
#define PUT_TIMEOUT_MS   15000
#define TX_TIMEOUT_MS    3000
/* The driver's buffers come out of scarce internal RAM, so they are small.
   A received upload line streams through RX (the task keeps reading it),
   and a long reply goes into TX in pieces. */
#define RX_RING_SIZE     4096
#define TX_RING_SIZE     2048
#define TX_PIECE         1024
#define MAX_ARGS         4
#define MAX_BUSY         8

extern void *rawsd;                      /* storage_tab5.c: SD card mounted */

static uint8_t s_rx[512];                /* received, not yet looked at */
static int s_rx_pos, s_rx_len;
static char *s_line;                     /* received line */
static int s_line_len;
static bool s_line_overflow;
static char *s_out;                      /* reply being built */
static uint8_t *s_data;                  /* file data */

static char *s_busy[MAX_BUSY];
static portMUX_TYPE s_busy_lock = portMUX_INITIALIZER_UNLOCKED;

/* --------------------------------------------------------------- helpers */

static uint32_t s_crc_table[256];

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        }
        s_crc_table[i] = c;
    }
}

/* zlib crc32(): start with 0 */
static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc = s_crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    }
    return ~crc;
}

static const char b64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_encode(const uint8_t *in, size_t n, char *out)
{
    char *o = out;
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        *o++ = b64_chars[v >> 18];
        *o++ = b64_chars[(v >> 12) & 63];
        *o++ = b64_chars[(v >> 6) & 63];
        *o++ = b64_chars[v & 63];
    }
    if (i < n) {
        uint32_t v = in[i] << 16;
        if (i + 1 < n) {
            v |= in[i + 1] << 8;
        }
        *o++ = b64_chars[v >> 18];
        *o++ = b64_chars[(v >> 12) & 63];
        *o++ = (i + 1 < n) ? b64_chars[(v >> 6) & 63] : '=';
        *o++ = '=';
    }
    *o = 0;
    return o - out;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* bytes decoded, or -1 on bad input or more than max */
static int b64_decode(const char *in, uint8_t *out, int max)
{
    int n = 0, bits = 0;
    uint32_t acc = 0;
    for (; *in && *in != '='; in++) {
        int v = b64_val(*in);
        if (v < 0) {
            return -1;
        }
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= max) {
                return -1;
            }
            out[n++] = acc >> bits;
        }
    }
    return n;
}

/* One reply line, written in one piece. Holding stdout's lock keeps log
   output out of it. False if the host stopped reading. */
bool sdfs_reply(const char *fmt, ...)
{
    int n = snprintf(s_out, LINE_MAX_LEN, "\r\n@fs ");
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(s_out + n, LINE_MAX_LEN - n - 2, fmt, ap);
    va_end(ap);
    if (n > LINE_MAX_LEN - 2) {
        n = LINE_MAX_LEN - 2;
    }
    s_out[n++] = '\r';
    s_out[n++] = '\n';
    flockfile(stdout);
    fflush(stdout);
    bool ok = true;
    for (int off = 0; ok && off < n; off += TX_PIECE) {
        int len = n - off < TX_PIECE ? n - off : TX_PIECE;
        ok = usb_serial_jtag_write_bytes(s_out + off, len, pdMS_TO_TICKS(TX_TIMEOUT_MS)) == len;
    }
    funlockfile(stdout);
    return ok;
}

/* Next "@fs ..." line into s_line (without the prefix); false on timeout.
   Other lines are dropped. */
static bool read_command(TickType_t timeout, char **cmd)
{
    TickType_t start = xTaskGetTickCount();
    for (;;) {
        if (s_rx_pos == s_rx_len) {
            TickType_t left = timeout;
            if (timeout != portMAX_DELAY) {
                TickType_t spent = xTaskGetTickCount() - start;
                if (spent >= timeout) {
                    return false;
                }
                left = timeout - spent;
            }
            int n = usb_serial_jtag_read_bytes(s_rx, sizeof(s_rx), left);
            s_rx_pos = 0;
            s_rx_len = n > 0 ? n : 0;
            continue;
        }
        uint8_t c = s_rx[s_rx_pos++];
        if (c == '\r' || c == '\n') {
            bool over = s_line_overflow;
            int len = s_line_len;
            s_line_len = 0;
            s_line_overflow = false;
            if (len == 0) {
                continue;
            }
            s_line[len] = 0;
            if (strncmp(s_line, "@fs ", 4) != 0) {
                continue;
            }
            if (over) {
                sdfs_reply("err line too long");
                continue;
            }
            *cmd = s_line + 4;
            return true;
        }
        if (s_line_len < LINE_MAX_LEN - 1) {
            s_line[s_line_len++] = c;
        } else {
            s_line_overflow = true;
        }
    }
}

/* split in place: spaces, "quoted args" */
static int split_args(char *s, char **argv, int max)
{
    int argc = 0;
    for (;;) {
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        if (!*s) {
            break;
        }
        if (argc == max) {
            return -1;
        }
        if (*s == '"') {
            argv[argc++] = ++s;
            while (*s && *s != '"') {
                s++;
            }
        } else {
            argv[argc++] = s;
            while (*s && *s != ' ' && *s != '\t') {
                s++;
            }
        }
        if (*s) {
            *s++ = 0;
        }
    }
    return argc;
}

/* card path for a name relative to the root ("/" and "/sdcard/" prefixes are
   accepted too); false for ".." or a backslash */
static bool card_path(const char *rel, char *out, size_t size)
{
    if (strncmp(rel, SDFS_ROOT "/", sizeof(SDFS_ROOT)) == 0) {
        rel += sizeof(SDFS_ROOT);
    }
    while (*rel == '/') {
        rel++;
    }
    if (strchr(rel, '\\')) {
        return false;
    }
    for (const char *p = rel; *p;) {
        const char *e = strchr(p, '/');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == 2 && p[0] == '.' && p[1] == '.') {
            return false;
        }
        p += len;
        while (*p == '/') {
            p++;
        }
    }
    int n = snprintf(out, size, "%s%s%s", SDFS_ROOT, *rel ? "/" : "", rel);
    /* no trailing '/' (FatFs) */
    while (n > (int)sizeof(SDFS_ROOT) && out[n - 1] == '/') {
        out[--n] = 0;
    }
    return n < (int)size;
}

void sdfs_set_busy(const char *const *paths, int n)
{
    char *copy[MAX_BUSY] = { 0 };
    int k = 0;
    for (int i = 0; i < n && k < MAX_BUSY; i++) {
        if (paths[i] && paths[i][0]) {
            copy[k++] = strdup(paths[i]);
        }
    }
    portENTER_CRITICAL(&s_busy_lock);
    for (int i = 0; i < MAX_BUSY; i++) {
        char *old = s_busy[i];
        s_busy[i] = copy[i];
        copy[i] = old;
    }
    portEXIT_CRITICAL(&s_busy_lock);
    for (int i = 0; i < MAX_BUSY; i++) {
        free(copy[i]);
    }
}

/* FAT names are case-insensitive */
static bool is_busy(const char *path)
{
    bool busy = false;
    portENTER_CRITICAL(&s_busy_lock);
    for (int i = 0; i < MAX_BUSY; i++) {
        if (s_busy[i] && strcasecmp(s_busy[i], path) == 0) {
            busy = true;
        }
    }
    portEXIT_CRITICAL(&s_busy_lock);
    return busy;
}

/* -------------------------------------------------------------- commands */

static void cmd_ls(int argc, char **argv)
{
    char path[300];
    if (!card_path(argc > 1 ? argv[1] : "", path, sizeof(path))) {
        sdfs_reply("err bad path");
        return;
    }
    DIR *d = opendir(path);
    if (!d) {
        sdfs_reply("err %s: %s", path, strerror(errno));
        return;
    }
    struct dirent *e;
    char full[600];
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st;
        if (e->d_type == DT_DIR) {
            sdfs_reply("D %s", e->d_name);
        } else if (stat(full, &st) == 0) {
            sdfs_reply("F %lld %s", (long long)st.st_size, e->d_name);
        } else {
            sdfs_reply("F -1 %s", e->d_name);
        }
    }
    closedir(d);
    sdfs_reply("ok");
}

static void cmd_get(int argc, char **argv)
{
    char path[300];
    if (argc != 2) {
        sdfs_reply("err usage: get <file>");
        return;
    }
    if (!card_path(argv[1], path, sizeof(path))) {
        sdfs_reply("err bad path");
        return;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        sdfs_reply("err %s: %s", path, strerror(errno));
        return;
    }
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || S_ISDIR(st.st_mode)) {
        fclose(f);
        sdfs_reply("err %s: not a file", path);
        return;
    }
    sdfs_reply("size %lld", (long long)st.st_size);
    uint32_t crc = 0;
    char *b64 = (char *)s_data + SDFS_GET_CHUNK;
    size_t n;
    while ((n = fread(s_data, 1, SDFS_GET_CHUNK, f)) > 0) {
        crc = crc32_update(crc, s_data, n);
        b64_encode(s_data, n, b64);
        if (!sdfs_reply("d %s", b64)) {
            fclose(f);
            ESP_LOGW(TAG, "get %s: host stopped reading", path);
            return;
        }
    }
    bool err = ferror(f);
    fclose(f);
    if (err) {
        sdfs_reply("err read error");
    } else {
        sdfs_reply("ok %08" PRIx32, crc);
    }
}

static void cmd_put(int argc, char **argv)
{
    char path[300], part[310];
    char *end;
    if (argc != 3) {
        sdfs_reply("err usage: put <file> <size>");
        return;
    }
    if (!card_path(argv[1], path, sizeof(path))) {
        sdfs_reply("err bad path");
        return;
    }
    long long size = strtoll(argv[2], &end, 10);
    if (*end || size < 0 || size > 0xffffffffLL) {
        sdfs_reply("err bad size");
        return;
    }
    if (is_busy(path)) {
        sdfs_reply("err %s is in use by the emulator", path);
        return;
    }
    snprintf(part, sizeof(part), "%s.part", path);
    FILE *f = fopen(part, "wb");
    if (!f) {
        sdfs_reply("err %s: %s", part, strerror(errno));
        return;
    }
    ESP_LOGI(TAG, "put %s (%lld bytes)", path, size);
    sdfs_reply("ready");

    long long got = 0;
    uint32_t crc = 0;
    const char *why = NULL;
    for (;;) {
        char *cmd;
        if (!read_command(pdMS_TO_TICKS(PUT_TIMEOUT_MS), &cmd)) {
            why = "timed out";
            break;
        }
        if (strncmp(cmd, "d ", 2) == 0) {
            int n = b64_decode(cmd + 2, s_data, SDFS_PUT_CHUNK);
            if (n < 0 || got + n > size) {
                why = "bad data line";
                break;
            }
            if (fwrite(s_data, 1, n, f) != (size_t)n) {
                why = strerror(errno);
                break;
            }
            crc = crc32_update(crc, s_data, n);
            got += n;
            sdfs_reply("ack %lld", got);
        } else if (strncmp(cmd, "end ", 4) == 0) {
            uint32_t want = strtoul(cmd + 4, NULL, 16);
            if (got != size) {
                why = "short upload";
            } else if (want != crc) {
                why = "CRC mismatch";
            }
            break;
        } else {
            why = "aborted";
            break;
        }
    }
    if (fclose(f) != 0 && !why) {
        why = strerror(errno);
    }
    if (!why && is_busy(path)) {
        why = "in use by the emulator";
    }
    if (!why) {
        struct stat st;
        if (stat(path, &st) == 0 && remove(path) != 0) {
            why = strerror(errno);
        } else if (rename(part, path) != 0) {
            why = strerror(errno);
        }
    }
    if (why) {
        remove(part);
        ESP_LOGW(TAG, "put %s: %s", path, why);
        sdfs_reply("err %s", why);
        return;
    }
    sdfs_reply("ok");
}

static void cmd_mv(int argc, char **argv)
{
    char from[300], to[300];
    if (argc != 3) {
        sdfs_reply("err usage: mv <old> <new>");
        return;
    }
    if (!card_path(argv[1], from, sizeof(from)) || !card_path(argv[2], to, sizeof(to))) {
        sdfs_reply("err bad path");
        return;
    }
    if (is_busy(from) || is_busy(to)) {
        sdfs_reply("err in use by the emulator");
        return;
    }
    struct stat st;
    if (stat(to, &st) == 0) {
        sdfs_reply("err %s exists", to);
        return;
    }
    if (rename(from, to) != 0) {
        sdfs_reply("err %s: %s", from, strerror(errno));
        return;
    }
    sdfs_reply("ok");
}

static void sdfs_task(void *arg)
{
    for (;;) {
        char *cmd;
        if (!read_command(portMAX_DELAY, &cmd)) {
            continue;
        }
        char *argv[MAX_ARGS];
        int argc = split_args(cmd, argv, MAX_ARGS);
        if (argc <= 0) {
            sdfs_reply("err bad command");
            continue;
        }
        if (strcmp(argv[0], "ping") == 0) {
            sdfs_reply("pong %s", argc > 1 ? argv[1] : "");
        } else if (net_command(argc, argv)) {
            /* Wi-Fi, timezone, reboot (net_tab5.c) */
        } else if (!rawsd) {
            sdfs_reply("err no SD card");
        } else if (strcmp(argv[0], "ls") == 0) {
            cmd_ls(argc, argv);
        } else if (strcmp(argv[0], "get") == 0) {
            cmd_get(argc, argv);
        } else if (strcmp(argv[0], "put") == 0) {
            cmd_put(argc, argv);
        } else if (strcmp(argv[0], "mv") == 0) {
            cmd_mv(argc, argv);
        } else if (strcmp(argv[0], "d") == 0 || strcmp(argv[0], "end") == 0) {
            /* rest of an upload that already failed */
        } else {
            sdfs_reply("err unknown command %s", argv[0]);
        }
    }
}

void sdfs_serial_start(void)
{
    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = RX_RING_SIZE,
        .tx_buffer_size = TX_RING_SIZE,
    };
    s_line = heap_caps_malloc(LINE_MAX_LEN, MALLOC_CAP_SPIRAM);
    s_out = heap_caps_malloc(LINE_MAX_LEN, MALLOC_CAP_SPIRAM);
    s_data = heap_caps_malloc(SDFS_PUT_CHUNK + SDFS_GET_CHUNK * 4 / 3 + 8, MALLOC_CAP_SPIRAM);
    if (!s_line || !s_out || !s_data) {
        ESP_LOGE(TAG, "out of memory");
        return;
    }
    esp_err_t e = usb_serial_jtag_driver_install(&cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "USB Serial/JTAG driver: %s", esp_err_to_name(e));
        return;
    }
    /* console output through the driver too, so both share its TX buffer */
    usb_serial_jtag_vfs_use_driver();
    crc_init();
    xTaskCreatePinnedToCore(sdfs_task, "sdfs", 6144, NULL, 2, NULL, 0);
}
