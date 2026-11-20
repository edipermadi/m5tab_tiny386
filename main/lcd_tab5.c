/*
 * Tab5 display for tiny386: brings up the panel with the BSP and copies the
 * emulator's frame buffer into the DPI panel's own frame buffer.
 *
 * Both buffers are 720x1280 RGB565 in the panel's portrait layout. tiny386 is
 * built with SWAPXY, so it writes the landscape picture transposed; one mirror
 * during the copy turns that transpose into a 90 or 270 degree rotation:
 *   TAB5_ROTATION_90  : every row reversed (mirror X)
 *   TAB5_ROTATION_270 : row order reversed (mirror Y)
 * The copy goes strip by strip; each strip is then written back from the cache
 * by esp_lcd_panel_draw_bitmap(), which for a pointer inside the panel's frame
 * buffer only does the cache write-back.
 */
#include <string.h>
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

void lcd_present_strip(const uint16_t *src_fb, int strip)
{
    if (!s_panel_fb) {
        return;
    }
    int y0 = strip * STRIP_ROWS;
    uint16_t *dst = s_panel_fb + (size_t)y0 * FB_W;
    for (int r = 0; r < STRIP_ROWS; r++) {
        int y = y0 + r;
#if CONFIG_TAB5_ROTATION_90
        int sy = y;
#else
        int sy = FB_H - 1 - y;
#endif
        copy_row(dst + (size_t)r * FB_W, src_fb + (size_t)sy * FB_W);
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
