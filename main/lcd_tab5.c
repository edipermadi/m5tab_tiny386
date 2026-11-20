/*
 * Tab5 display for tiny386: brings up the panel with the BSP and copies the
 * emulator's frame buffer into the DPI panel's own frame buffer.
 *
 * Both buffers are 720x1280 RGB565 in the panel's portrait layout. tiny386 is
 * built with SWAPXY, so it writes the landscape picture transposed; one mirror
 * during the copy turns that transpose into a 90 or 270 degree rotation:
 *   TAB5_ROTATION_90  : every row reversed (mirror X)
 *   TAB5_ROTATION_270 : row order reversed (mirror Y)
 * The copy also scales the guest's picture up to fill the panel (see
 * build_maps()). The copy goes strip by strip; each strip is then written back from the cache
 * by esp_lcd_panel_draw_bitmap(), which for a pointer inside the panel's frame
 * buffer only does the cache write-back.
 */
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"

#include "bsp/bsp_board.h"
#include "tab5.h"
#include "common.h"
#include "../tiny386/pc.h"
#include "hdd_cache.h"

static const char *TAG = "lcd";

#define FB_W LCD_HEIGHT   /* 720, panel row length */
#define FB_H LCD_WIDTH    /* 1280, panel rows */
#define NSTRIPS 32
#define STRIP_ROWS (FB_H / NSTRIPS)

_Static_assert(FB_W == BSP_LCD_H_RES && FB_H == BSP_LCD_V_RES, "panel size");
_Static_assert(FB_H % NSTRIPS == 0, "strip size");

static uint16_t *s_panel_fb;

/*
 * Scaling. tiny386 draws the guest's picture 1:1, centred in the 1280x720
 * landscape frame buffer. Here it is scaled up to the panel's full height:
 *   - modes up to 400 lines (text, 320x200, 640x350/400) were shown 4:3 on a
 *     VGA monitor, so they are shown 4:3 here (960x720) too;
 *   - taller modes have square pixels (640x480 and 1024x768 to 960x720,
 *     1280x720 as it is).
 * The ratios are not whole numbers (720x400 text: 1.33 across, 1.8 down), so
 * plain nearest neighbour makes some source pixels one panel pixel wide and
 * others two, and text looks uneven. The sharp filter gives every source
 * pixel the same width and blends only the panel pixels that straddle two
 * (area sampling): edges stay crisp, strokes stay even.
 *
 * Maps, rebuilt when the picture's size changes:
 *   s_row[py], s_roww[py]: panel row -> first source row (landscape x), -1 for
 *       black, and the weight (0..32) of the source row after it
 *   s_col[pc], s_colw[pc]: the same for panel columns (landscape y)
 */
#ifdef CONFIG_TAB5_SCALE_OFF
#define SCALE_OFF 1
#else
#define SCALE_OFF 0
#endif
#define WBITS 5
#define WONE (1 << WBITS)

static int16_t s_row[FB_H];
static uint8_t s_roww[FB_H];
static int16_t s_col[FB_W];
static uint8_t s_colw[FB_W];
static int s_pic_w = -1, s_pic_h = -1;
static bool s_identity = true;

/*
 * Disk activity LEDs, drawn in the black bars left and right of a picture
 * narrower than the panel, near the bottom: hard disk on the left (green
 * read, amber write), CD-ROM on the right (green read), dim grey when idle.
 * Each access keeps its LED lit for LED_HOLD_MS so short ones show.
 */
#define LED_SIZE     20     /* square, landscape pixels */
#define LED_BOTTOM   40     /* gap below it */
#define LED_HOLD_MS  80
#define LED_IDLE     0x2945 /* dim grey */
#define LED_READ     0x07E0 /* green */
#define LED_WRITE    0xFD20 /* amber */

static int s_bar_w;         /* width of each side bar; 0: no room for LEDs */
static uint16_t s_led[2];   /* this frame's colours: [0] left/HDD, [1] right/CD */

/* panel pixel u of n over a source run of m pixels (n >= m): first source
   pixel and the weight of the next one */
static void area_map(int u, int n, int m, int base, int16_t *idx, uint8_t *wt)
{
    int c0 = u * m / n;
    int w = 0;
#if CONFIG_TAB5_SCALE_SHARP
    /* panel pixel u covers [u*m, (u+1)*m) in units of 1/n source pixel */
    int edge = (c0 + 1) * n;
    if (edge < (u + 1) * m && c0 + 1 < m) {
        w = ((u + 1) * m - edge) * WONE / m;
        if (w >= WONE) {
            w = WONE - 1;
        }
    }
#endif
    *idx = base + c0;
    *wt = w;
}

static void build_maps(int w, int h)
{
    const int LW = FB_H, LH = FB_W; /* landscape size: 1280 x 720 */
    int ow = LW, oh = LH;           /* scaled picture size */

    s_identity = (SCALE_OFF || w <= 0 || h <= 0 || w >= LW || h >= LH);
    if (!s_identity) {
        /* aspect ratio as num/den */
        int num = h <= 400 ? 4 : w, den = h <= 400 ? 3 : h;
        oh = LH;
        ow = LH * num / den;
        if (ow > LW) {
            ow = LW;
            oh = LW * den / num;
        }
        if (ow <= w && oh <= h) { /* would not grow: leave it */
            s_identity = true;
        }
    }
    if (s_identity) {
        s_bar_w = (w > 0 && w < LW) ? (LW - w) / 2 : 0;
        if (s_bar_w < LED_SIZE + 8) {
            s_bar_w = 0;
        }
        ESP_LOGI(TAG, "picture %dx%d: unscaled", w, h);
        return;
    }
    s_bar_w = (LW - ow) / 2;
    if (s_bar_w < LED_SIZE + 8) {
        s_bar_w = 0;
    }
    ESP_LOGI(TAG, "picture %dx%d: scaled to %dx%d", w, h, ow, oh);

    int x0 = (LW - w) / 2, y0 = (LH - h) / 2;     /* where tiny386 put it */
    int ox0 = (LW - ow) / 2, oy0 = (LH - oh) / 2; /* where it goes */
    for (int py = 0; py < FB_H; py++) {
#if CONFIG_TAB5_ROTATION_90
        int X = py;
#else
        int X = LW - 1 - py;
#endif
        int u = X - ox0;
        if (u < 0 || u >= ow) {
            s_row[py] = -1;
            s_roww[py] = 0;
        } else {
            area_map(u, ow, w, x0, &s_row[py], &s_roww[py]);
        }
    }
    for (int pc = 0; pc < FB_W; pc++) {
#if CONFIG_TAB5_ROTATION_90
        int Y = LH - 1 - pc;
#else
        int Y = pc;
#endif
        int v = Y - oy0;
        if (v < 0 || v >= oh) {
            s_col[pc] = -1;
            s_colw[pc] = 0;
        } else {
            area_map(v, oh, h, y0, &s_col[pc], &s_colw[pc]);
        }
    }
}

/* RGB565 blend: the channels spread apart in 32 bits, with room to multiply */
static inline uint32_t spread(uint32_t c)
{
    return (c | (c << 16)) & 0x07E0F81F;
}

static inline uint16_t blend(uint16_t a, uint16_t b, int w)
{
    uint32_t x = (spread(a) * (WONE - w) + spread(b) * w) >> WBITS;
    x &= 0x07E0F81F;
    return (uint16_t)(x | (x >> 16));
}

static inline void copy_row(uint16_t *restrict dst, const uint16_t *restrict src)
{
#if CONFIG_TAB5_ROTATION_90
    const uint16_t *s = src + FB_W;
    for (int x = 0; x < FB_W; x++) {
        dst[x] = *--s;
    }
#else
    memcpy(dst, src, FB_W * sizeof(uint16_t));
#endif
}

/* one source row scaled along the panel row, into internal RAM */
static void scale_row(uint16_t *restrict dst, const uint16_t *restrict src)
{
    for (int x = 0; x < FB_W; x++) {
        int c = s_col[x];
        if (c < 0) {
            dst[x] = 0;
        } else if (!s_colw[x]) {
            dst[x] = src[c];
        } else {
            dst[x] = blend(src[c], src[c + 1], s_colw[x]);
        }
    }
}

/* the last two scaled source rows: consecutive panel rows mostly share them */
static uint16_t s_line[2][FB_W];
static int s_line_src[2] = { -1, -1 };

static const uint16_t *scaled_line(const uint16_t *src_fb, int r, int keep)
{
    for (int i = 0; i < 2; i++) {
        if (s_line_src[i] == r) {
            return s_line[i];
        }
    }
    int i = (s_line_src[0] == keep) ? 1 : 0;
    scale_row(s_line[i], src_fb + (size_t)r * FB_W);
    s_line_src[i] = r;
    return s_line[i];
}

static void led_update(void)
{
    if (!s_bar_w) {
        return;
    }
    s_led[0] = hdd_idle_ms(false, true) < LED_HOLD_MS  ? LED_WRITE
             : hdd_idle_ms(false, false) < LED_HOLD_MS ? LED_READ : LED_IDLE;
    s_led[1] = hdd_idle_ms(true, false) < LED_HOLD_MS ? LED_READ : LED_IDLE;
}

/* the LEDs' part of panel row y (already drawn: bar pixels are black) */
static void led_draw(uint16_t *d, int y)
{
    const int LW = FB_H, LH = FB_W;
#if CONFIG_TAB5_ROTATION_90
    int X = y;
#else
    int X = LW - 1 - y;
#endif
    int lx = s_bar_w / 2 - LED_SIZE / 2;
    int side;
    if (X >= lx && X < lx + LED_SIZE) {
        side = 0;
    } else if (X >= LW - lx - LED_SIZE && X < LW - lx) {
        side = 1;
    } else {
        return;
    }
    int y1 = LH - LED_BOTTOM, y0 = y1 - LED_SIZE; /* landscape rows [y0, y1) */
#if CONFIG_TAB5_ROTATION_90
    int c0 = LH - y1, c1 = LH - y0;
#else
    int c0 = y0, c1 = y1;
#endif
    for (int c = c0; c < c1; c++) {
        d[c] = s_led[side];
    }
}

void lcd_present_strip(const uint16_t *src_fb, int strip)
{
    if (!s_panel_fb) {
        return;
    }
    if (strip == 0) {
        if (globals.pc) {
            int w, h;
            vga_get_picture_size(((PC *)globals.pc)->vga, &w, &h);
            if (w != s_pic_w || h != s_pic_h) {
                s_pic_w = w;
                s_pic_h = h;
                build_maps(w, h);
            }
        }
        s_line_src[0] = s_line_src[1] = -1; /* a new frame */
        led_update();
    }
    int y0 = strip * STRIP_ROWS;
    uint16_t *dst = s_panel_fb + (size_t)y0 * FB_W;
    for (int r = 0; r < STRIP_ROWS; r++) {
        int y = y0 + r;
        uint16_t *d = dst + (size_t)r * FB_W;
        if (s_identity) {
#if CONFIG_TAB5_ROTATION_90
            int sy = y;
#else
            int sy = FB_H - 1 - y;
#endif
            copy_row(d, src_fb + (size_t)sy * FB_W);
            if (s_bar_w) {
                led_draw(d, y);
            }
            continue;
        }
        int sr = s_row[y], w = s_roww[y];
        if (sr < 0) {
            memset(d, 0, FB_W * sizeof(uint16_t));
            if (s_bar_w) {
                led_draw(d, y); /* (LEDs are only ever in the bars) */
            }
        } else if (r > 0 && sr == s_row[y - 1] && w == s_roww[y - 1]) {
            memcpy(d, d - FB_W, FB_W * sizeof(uint16_t)); /* a repeated line */
        } else if (!w) {
            memcpy(d, scaled_line(src_fb, sr, -1), FB_W * sizeof(uint16_t));
        } else {
            /* the next source row: one step on along landscape x, which runs
               down or up the panel's rows with the rotation */
            const uint16_t *a = scaled_line(src_fb, sr, -1);
            const uint16_t *b = scaled_line(src_fb, sr + 1, sr);
            for (int x = 0; x < FB_W; x++) {
                d[x] = blend(a[x], b[x], w);
            }
        }
    }
    esp_lcd_panel_draw_bitmap(globals.panel, 0, y0, FB_W, y0 + STRIP_ROWS, dst);
}

int lcd_strip_count(void)
{
    return NSTRIPS;
}

void vga_task(void *arg)
{
    ESP_LOGI(TAG, "vga runs on core %d", esp_cpu_get_core_id());

    esp_lcd_panel_handle_t panel = NULL;
    esp_err_t err = bsp_display_new(&panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "panel: %s", bsp_display_panel_name());

    void *fb = NULL;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb));
    s_panel_fb = fb;
    memset(s_panel_fb, 0, (size_t)FB_W * FB_H * 2);
    esp_lcd_panel_draw_bitmap(panel, 0, 0, FB_W, FB_H, s_panel_fb);
    bsp_display_brightness_set(CONFIG_TAB5_BACKLIGHT_PERCENT);

    globals.panel = panel;
    /* tell i386_task that the panel is ready */
    xEventGroupSetBits(global_event_group, BIT1);
    xEventGroupWaitBits(global_event_group, BIT0, pdFALSE, pdFALSE, portMAX_DELAY);

    for (;;) {
        pc_vga_step(globals.pc);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
