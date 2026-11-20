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
 * landscape frame buffer. Here it is scaled up, nearest neighbour, to the
 * panel's full height:
 *   - modes up to 400 lines (text, 320x200, 640x350/400) were shown 4:3 on a
 *     VGA monitor, so they are shown 4:3 here (960x720) too;
 *   - taller modes have square pixels (640x480 and 1024x768 to 960x720,
 *     1280x720 as it is).
 * Two maps, rebuilt when the picture's size changes:
 *   s_row[py]: panel row -> source row (landscape x), -1 for black
 *   s_col[pc]: panel column -> source column (landscape y), -1 for black
 */
static int16_t s_row[FB_H];
static int16_t s_col[FB_W];
static int s_pic_w = -1, s_pic_h = -1;
static bool s_identity = true;

static void build_maps(int w, int h)
{
    const int LW = FB_H, LH = FB_W; /* landscape size: 1280 x 720 */
    int ow = LW, oh = LH;           /* scaled picture size */

    s_identity = (w <= 0 || h <= 0 || w >= LW || h >= LH);
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
        ESP_LOGI(TAG, "picture %dx%d: unscaled", w, h);
        return;
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
        s_row[py] = (u < 0 || u >= ow) ? -1 : x0 + u * w / ow;
    }
    for (int pc = 0; pc < FB_W; pc++) {
#if CONFIG_TAB5_ROTATION_90
        int Y = LH - 1 - pc;
#else
        int Y = pc;
#endif
        int v = Y - oy0;
        s_col[pc] = (v < 0 || v >= oh) ? -1 : y0 + v * h / oh;
    }
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

static inline void scale_row(uint16_t *restrict dst, const uint16_t *restrict src)
{
    for (int x = 0; x < FB_W; x++) {
        int c = s_col[x];
        dst[x] = c < 0 ? 0 : src[c];
    }
}

void lcd_present_strip(const uint16_t *src_fb, int strip)
{
    if (!s_panel_fb) {
        return;
    }
    if (strip == 0 && globals.pc) {
        int w, h;
        vga_get_picture_size(((PC *)globals.pc)->vga, &w, &h);
        if (w != s_pic_w || h != s_pic_h) {
            s_pic_w = w;
            s_pic_h = h;
            build_maps(w, h);
        }
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
        } else if (s_row[y] < 0) {
            memset(d, 0, FB_W * sizeof(uint16_t));
        } else if (r > 0 && s_row[y] == s_row[y - 1]) {
            memcpy(d, d - FB_W, FB_W * sizeof(uint16_t)); /* a repeated line */
        } else {
            scale_row(d, src_fb + (size_t)s_row[y] * FB_W);
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
