/*
 * tiny386 on the M5Stack Tab5. Adapted from tiny386 esp/main/esp_main.c.
 *
 * Tasks:
 *   i386_main (core 1)  the emulated PC
 *   vga_task  (core 0)  panel bring-up, VGA refresh -> panel
 *   audio     (core 0)  mixer -> ES8388
 *   tab5_kbd  (core 0)  Tab5 Keyboard (I2C) -> PS/2 keyboard
 *   usb_*     (core 0)  USB host HID (mouse, keyboard) on the USB-A port
 *   battery   (core 0)  INA226 -> the guest's APM power status
 *   sdfs      (core 0)  SD card file access, settings over the USB-C console
 *   (at boot only: ESP-Hosted, Wi-Fi, SNTP for the clock, net_tab5.c)
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
#include "esp_sleep.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "bsp/bsp_codec.h"

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

/* CMOS through its own ports (the struct is private to tiny386's misc.c) */
static uint8_t cmos_rd(CMOS *c, int reg)
{
    cmos_ioport_write(c, 0x70, reg);
    return cmos_ioport_read(c, 0x71);
}

static void cmos_wr(CMOS *c, int reg, uint8_t val)
{
    cmos_ioport_write(c, 0x70, reg);
    cmos_ioport_write(c, 0x71, val);
}

/*
 * tiny386 puts the memory size only in CMOS 0x30/0x31 (0x34/0x35 from 64 MB):
 * enough for SeaBIOS, but the standard base and extended memory registers stay
 * 0, with no checksum over them. Windows 95 setup reads those and stops with
 * "not enough memory". Fill them in as a real BIOS setup would.
 */
static void cmos_fill_memory(CMOS *c, long mem_size)
{
    long ext_kb = mem_size / 1024 - 1024;
    if (ext_kb < 0) {
        ext_kb = 0;
    } else if (ext_kb > 0xffff) {
        ext_kb = 0xffff;
    }
    cmos_wr(c, 0x15, 640 & 0xff); /* base memory, KB */
    cmos_wr(c, 0x16, 640 >> 8);
    cmos_wr(c, 0x17, ext_kb & 0xff); /* extended memory, KB */
    cmos_wr(c, 0x18, ext_kb >> 8);
    if (!cmos_rd(c, 0x30) && !cmos_rd(c, 0x31)) { /* (left 0 from 64 MB) */
        cmos_wr(c, 0x30, ext_kb & 0xff);
        cmos_wr(c, 0x31, ext_kb >> 8);
    }
    unsigned sum = 0; /* checksum of 0x10-0x2d, big-endian in 0x2e/0x2f */
    for (int r = 0x10; r <= 0x2d; r++) {
        sum += cmos_rd(c, r);
    }
    cmos_wr(c, 0x2e, sum >> 8);
    cmos_wr(c, 0x2f, sum & 0xff);
    cmos_ioport_write(c, 0x70, 0);
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

#if CONFIG_TAB5_SERIAL_FS
    {
        const char *busy[] = {
            conf.disks[0], conf.disks[1], conf.disks[2], conf.disks[3],
            conf.fdd[0], conf.fdd[1],
        };
        sdfs_set_busy(busy, sizeof(busy) / sizeof(busy[0]));
    }
#endif

    Console *console = console_init();
    PC *pc = pc_new(redraw, console, console->fb, &conf);
    console->pc = pc;
    cmos_fill_memory(pc->cmos, conf.mem_size);
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

/* --------------------------------------------------------------- battery */
/*
 * The Tab5's battery, as the guest sees it through APM "get power status"
 * (the patched SeaBIOS reads PC.host_power at port 0x8902): charge level,
 * charging, and AC (USB-C) power. There is no USB-C VBUS sense, so AC is
 * inferred from the battery current: on battery alone the board draws far
 * more than 10 mA from it.
 */
#define BATTERY_POLL_MS     5000
#define BATTERY_IDLE_MA     10      /* M5Unified's charging/discharging threshold */

static void battery_task(void *arg)
{
    xEventGroupWaitBits(global_event_group, BIT0, pdFALSE, pdFALSE, portMAX_DELAY);
    PC *pc = globals.pc;
    uint16_t last = PC_POWER_DEFAULT;
    for (;;) {
        bsp_battery_t bat = { 0 };
        uint16_t st = PC_POWER_DEFAULT;
        if (bsp_battery_read(&bat) == ESP_OK && bat.present) {
            st = bat.percent;
            if (bat.current_ma >= -BATTERY_IDLE_MA) {
                st |= PC_POWER_AC_ONLINE;
            }
            if (bat.current_ma > BATTERY_IDLE_MA) {
                st |= PC_POWER_CHARGING;
            }
        }
        pc->host_power = st;
        if ((st & ~PC_POWER_PERCENT_MASK) != (last & ~PC_POWER_PERCENT_MASK)) {
            ESP_LOGI(TAG, "power: %s, %s%s, %d %% (%d mV/cell, %d mA)",
                     (st & PC_POWER_NO_BATTERY) ? "no battery" : "battery",
                     (st & PC_POWER_AC_ONLINE) ? "AC" : "on battery",
                     (st & PC_POWER_CHARGING) ? ", charging" : "",
                     st & PC_POWER_PERCENT_MASK, bat.cell_mv, bat.current_ma);
        }
        last = st;
        vTaskDelay(pdMS_TO_TICKS(BATTERY_POLL_MS));
    }
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

#if CONFIG_TAB5_POWEROFF_ON_SHUTDOWN
/*
 * The guest has shut down (APM "off": Windows' "Shut down", or any DOS power-
 * off tool; tiny386's SeaBIOS then writes "Shutdown" to port 0x8900 and
 * pc_main() returns). Disk writes are already on the card (hdd_cache writes
 * through), so everything can go: speaker, panel and backlight off, the
 * expander-switched rails off, then PWROFF_PULSE. On battery that cuts the
 * power (the side power button turns it back on). On USB the board stays
 * powered, so it deep sleeps with no wake-up source and the reset button
 * starts it again.
 */
static void power_off_board(void)
{
    ESP_LOGW(TAG, "guest shut down: everything off, deep sleep (reset = power on)");

    bsp_codec_mute(true);
    bsp_audio_amp_set(false);

    bsp_display_brightness_set(0);
    if (globals.panel) {
        esp_lcd_panel_disp_on_off(globals.panel, false);
        esp_lcd_panel_disp_sleep(globals.panel, true);
        vTaskDelay(pdMS_TO_TICKS(120)); /* sleep-in takes up to 120 ms */
    }
    /* backlight enable low through deep sleep (held where the chip can:
       P4 rev >= 3.0; earlier ones leave the pin to the board's pull-down) */
    gpio_reset_pin(BSP_LCD_BACKLIGHT_GPIO);
    gpio_set_direction(BSP_LCD_BACKLIGHT_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BSP_LCD_BACKLIGHT_GPIO, 0);
    gpio_pulldown_en(BSP_LCD_BACKLIGHT_GPIO);
    gpio_hold_en(BSP_LCD_BACKLIGHT_GPIO);

    bsp_power_down();

    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(20));
    /* on battery the supply is cut here; on USB we get past it */
    bsp_power_off();
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_deep_sleep_start();
}
#endif

static void i386_task(void *arg)
{
    struct esp_ini_config *config = arg;
    ESP_LOGI(TAG, "i386 runs on core %d", esp_cpu_get_core_id());
    /* wait for the panel */
    xEventGroupWaitBits(global_event_group, BIT1, pdFALSE, pdFALSE, portMAX_DELAY);
    pc_main(config->filename);
    ESP_LOGI(TAG, "PC powered off");
#if CONFIG_TAB5_POWEROFF_ON_SHUTDOWN
    power_off_board();
#endif
    vTaskDelete(NULL);
}

void app_main(void)
{
    global_event_group = xEventGroupCreate();

    /* SYS I2C + I/O expanders: LCD/touch reset, EXT5V (keyboard port), USB-A 5V */
    ESP_ERROR_CHECK(bsp_i2c_init(NULL));

    audio_start();
#if CONFIG_TAB5_SERIAL_FS
    /* first: settings can be changed while the clock is being set, and a
       tiny386.ini can be put on a card that has none */
    sdfs_serial_start();
#endif
    /* the panel first, for the status of the next step */
    lcd_init();
    /* the clock over Wi-Fi, then Wi-Fi off: before the SD card (the C6 link
       shares its SDMMC controller) and before USB (RAM) */
    net_init();
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
#if CONFIG_TAB5_TOUCHPAD
    touch_start();
#endif
    if (config.enable_usb) {
        usb_setup();
    }

    xTaskCreatePinnedToCore(i386_task, "i386_main", 8192, &config, 3, NULL, 1);
    xTaskCreatePinnedToCore(vga_task, "vga_task", 4096, NULL, 0, NULL, 0);
    xTaskCreatePinnedToCore(battery_task, "battery", 3072, NULL, 1, NULL, 0);

    ESP_LOGI(TAG, "free internal %u KB (DMA-capable %u KB, largest block %u KB), PSRAM %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) >> 10),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) >> 10),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10));
}
