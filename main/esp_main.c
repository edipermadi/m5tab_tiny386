/*
 * tiny386 on the M5Stack Tab5. Adapted from tiny386 esp/main/esp_main.c.
 *
 * Tasks:
 *   i386_main (core 1)  the emulated PC
 *   vga_task  (core 0)  panel bring-up, VGA refresh -> panel
 *   audio     (core 0)  mixer -> ES8388
 *   tab5_kbd  (core 0)  Tab5 Keyboard (I2C) -> PS/2 keyboard
 *   usb_*     (core 0)  USB host HID (mouse, keyboard) on the USB-A port
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"

#include "../tiny386/ini.h"
#include "../tiny386/pc.h"
#include "bsp/bsp_board.h"
#include "common.h"
#include "tab5.h"

static const char *TAG = "tiny386";

uint32_t get_uticks()
{
    return (uint32_t)esp_timer_get_time();
}

/* NE2000 transmit hook, set by tiny386's Wi-Fi glue (esp/main/wifi.c). No
 * Wi-Fi in this port yet, so the emulated NIC has no link. */
void (*_Atomic esp32_send_packet)(uint8_t *buf, int size);

/* ---------------------------------------------------------------- memory */
static char *psram;
static long psram_off;
static long psram_len;

void *psmalloc(long size)
{
    void *ret = psram + psram_off;

    size = (size + 4095) / 4096 * 4096;
    if (psram_off + size > psram_len) {
        fprintf(stderr, "psram error %ld %ld %ld\n", size, psram_off, psram_len);
        abort();
    }
    psram_off += size;
    return ret;
}

void *bigmalloc(size_t size)
{
    return psmalloc(size);
}

static char *pcram;
static long pcram_off;
static long pcram_len;

void *pcmalloc(long size)
{
    void *ret = pcram + pcram_off;

    size = (size + 31) / 32 * 32;
    if (pcram_off + size > pcram_len) {
        fprintf(stderr, "pcram error %ld %ld %ld\n", size, pcram_off, pcram_len);
        abort();
    }
    pcram_off += size;
    return ret;
}

void pcmalloc_init(void *ptr, long len)
{
    pcram = ptr;
    pcram_len = len;
}

/* ------------------------------------------------------------------ ROMs */
int load_rom(void *phys_mem, const char *file, uword addr, int backward)
{
    if (file && file[0] == '/') {
        FILE *fp = fopen(file, "rb");
        if (!fp) {
            fprintf(stderr, "cannot open %s\n", file);
            abort();
        }
        fseek(fp, 0, SEEK_END);
        int len = ftell(fp);
        fprintf(stderr, "%s len %d\n", file, len);
        rewind(fp);
        if (backward) {
            fread(phys_mem + addr - len, 1, len, fp);
        } else {
            fread(phys_mem + addr, 1, len, fp);
        }
        fclose(fp);
        return len;
    }
    /* otherwise: a flash partition of that name */
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, file);
    if (!part) {
        fprintf(stderr, "no file or partition '%s'\n", file ? file : "(null)");
        abort();
    }
    int len = part->size;
    fprintf(stderr, "%s len %d\n", file, len);
    if (backward) {
        esp_partition_read(part, 0, phys_mem + addr - len, len);
    } else {
        esp_partition_read(part, 0, phys_mem + addr, len);
    }
    return len;
}

/* --------------------------------------------------------------- console */
EventGroupHandle_t global_event_group;
struct Globals globals;

typedef struct {
    PC *pc;
    u8 *fb;
} Console;

static Console *console_init(void)
{
    Console *c = calloc(1, sizeof(Console));
    c->fb = bigmalloc(LCD_WIDTH * LCD_HEIGHT * 2);
    memset(c->fb, 0, LCD_WIDTH * LCD_HEIGHT * 2);
    return c;
}

static void redraw(void *opaque, int x, int y, int w, int h)
{
    Console *s = opaque;
    int n = lcd_strip_count();
    for (int i = 0; i < n; i++) {
        lcd_present_strip((const uint16_t *)s->fb, i);
        vga_step(s->pc->vga);
        usleep(900);
    }
}

static int pc_main(const char *file)
{
    PCConfig conf;
    memset(&conf, 0, sizeof(conf));
    conf.mem_size = 8 * 1024 * 1024;
    conf.vga_mem_size = 256 * 1024;
    conf.cpu_gen = 4;
    conf.fpu = 0;

    int err = ini_parse(file, parse_conf_ini, &conf);
    if (err) {
        fprintf(stderr, "error %d parsing %s\n", err, file);
        return err;
    }

    if (conf.width != LCD_WIDTH || conf.height != LCD_HEIGHT) {
        fprintf(stderr, "fixing width/height mismatch %dx%d => %dx%d\n",
                conf.width, conf.height, LCD_WIDTH, LCD_HEIGHT);
        conf.width = LCD_WIDTH;
        conf.height = LCD_HEIGHT;
    }

    Console *console = console_init();
    PC *pc = pc_new(redraw, console, console->fb, &conf);
    console->pc = pc;
    globals.pc = pc;
    globals.kbd = pc->kbd;
    globals.mouse = pc->mouse;
    xEventGroupSetBits(global_event_group, BIT0);

    load_bios_and_reset(pc);

    pc->boot_start_time = get_uticks();
    for (; pc->shutdown_state != 8;) {
        pc_step(pc);
    }
    return 0;
}

/* ------------------------------------------------------------------ init */
struct esp_ini_config {
    const char *filename;
    int enable_usb;
};

static int parse_ini(void *user, const char *section, const char *name, const char *value)
{
    struct esp_ini_config *conf = user;
    if (strcmp(section, "esp") == 0 && strcmp(name, "enable_usb") == 0) {
        conf->enable_usb = atoi(value);
    }
    return 1;
}

static void i386_task(void *arg)
{
    struct esp_ini_config *config = arg;
    ESP_LOGI(TAG, "i386 runs on core %d", esp_cpu_get_core_id());
    /* wait for the panel */
    xEventGroupWaitBits(global_event_group, BIT1, pdFALSE, pdFALSE, portMAX_DELAY);
    pc_main(config->filename);
    ESP_LOGI(TAG, "PC powered off");
    vTaskDelete(NULL);
}

void app_main(void)
{
    global_event_group = xEventGroupCreate();

    /* SYS I2C + I/O expanders: LCD/touch reset, EXT5V (keyboard port), USB-A 5V */
    ESP_ERROR_CHECK(bsp_i2c_init(NULL));

    audio_start();
    storage_init();

    psram_len = PSRAM_ALLOC_LEN;
    psram = heap_caps_calloc(1, psram_len, MALLOC_CAP_SPIRAM);
    if (!psram) {
        ESP_LOGE(TAG, "cannot allocate %ld bytes of PSRAM (free %u)", psram_len,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return;
    }

    static const char *files[] = {
        "/sdcard/tiny386.ini",
        "/spiflash/tiny386.ini",
        NULL,
    };
    static struct esp_ini_config config = {
        .enable_usb = CONFIG_TAB5_USB_HOST,
    };
    for (int i = 0; files[i]; i++) {
        if (ini_parse(files[i], parse_ini, &config) == 0) {
            config.filename = files[i];
            break;
        }
    }
    if (!config.filename) {
        ESP_LOGE(TAG, "no tiny386.ini on the SD card (or in the flash FAT)");
        return;
    }
    ESP_LOGI(TAG, "config: %s", config.filename);

    tab5_kbd_start();
    if (config.enable_usb) {
        usb_setup();
    }

    xTaskCreatePinnedToCore(i386_task, "i386_main", 8192, &config, 3, NULL, 1);
    xTaskCreatePinnedToCore(vga_task, "vga_task", 4096, NULL, 0, NULL, 0);

    ESP_LOGI(TAG, "free internal %u KB, PSRAM %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10));
}
