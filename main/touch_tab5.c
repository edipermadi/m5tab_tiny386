/*
 * Touch screen -> the emulated PS/2 mouse, as a touchpad. The guest needs no
 * driver: Windows and DOS see the PS/2 mouse they already know.
 *
 *   picture area   touchpad: slide to move the pointer, tap = left click,
 *                  two-finger tap = right click
 *   left bar       left button, held while touched (hold it and slide on the
 *                  picture to drag)
 *   right bar      right button, held while touched
 *
 * The bars are the black areas beside a picture narrower than the panel (see
 * lcd_tab5.c); a full-width picture has none, and then only the taps click.
 *
 * Touch controller: GT911 or ST712x, through bsp_touch.c (from the tested
 * esp32p4_86box BSP). It reports in the panel's portrait coordinates, which
 * are turned into the landscape ones the picture uses, as the display does.
 */
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "bsp/bsp_board.h"
#include "tab5.h"
#include "common.h"
#include "../tiny386/pc.h"

static const char *TAG = "touch";

#define LW 1280             /* landscape picture size */
#define LH 720

#define POLL_MS       10
#define MAX_POINTS    10
#define TAP_MAX_MS    200   /* a tap: lifted within this... */
#define TAP_MAX_PX    12    /* ...having moved less than this */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

typedef struct {
    int id;
    int x, y;   /* landscape */
} Point;

static void touch_task(void *arg)
{
    /* the PC (for its mouse) and the panel (for the bars) first */
    xEventGroupWaitBits(global_event_group, BIT0 | BIT1, pdFALSE, pdTRUE, portMAX_DELAY);
    PS2MouseState *mouse = ((PC *)globals.pc)->mouse;

    if (bsp_touch_init() != ESP_OK) {
        ESP_LOGE(TAG, "no touch controller: touchpad off");
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "touchpad on");

    int pad_id = -1, last_x = 0, last_y = 0;
    int acc_x = 0, acc_y = 0;               /* movement, in 1/100 counts */
    bool tap_on = false, tap_moved = false;
    int tap_fingers = 0, tap_x = 0, tap_y = 0;
    uint32_t tap_t = 0;
    int buttons = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        bsp_touch_point_t tp[MAX_POINTS];
        int n = bsp_touch_read_points(tp, MAX_POINTS);
        if (n < 0) {
            continue; /* nothing new */
        }

        /* sort the points into the bars and the touchpad */
        int bar = lcd_bar_width();
        int want = 0;
        Point pad[MAX_POINTS];
        int npad = 0;
        for (int i = 0; i < n; i++) {
            int px = tp[i].x; /* panel column */
            int py = tp[i].y; /* panel row */
#if CONFIG_TAB5_ROTATION_90
            int X = py, Y = LH - 1 - px;
#else
            int X = LW - 1 - py, Y = px;
#endif
            if (bar && X < bar) {
                want |= 1;
            } else if (bar && X >= LW - bar) {
                want |= 2;
            } else {
                pad[npad++] = (Point){ tp[i].id, X, Y };
            }
        }

        /* touchpad: the finger that came first moves the pointer */
        int click = 0;
        if (npad) {
            const Point *f = NULL;
            for (int i = 0; i < npad; i++) {
                if (pad[i].id == pad_id) {
                    f = &pad[i];
                }
            }
            if (f) {
                acc_x += (f->x - last_x) * CONFIG_TAB5_TOUCHPAD_SPEED;
                acc_y += (f->y - last_y) * CONFIG_TAB5_TOUCHPAD_SPEED;
            } else {
                f = &pad[0];
                pad_id = f->id;
                if (!tap_on) {
                    tap_on = true;
                    tap_moved = false;
                    tap_fingers = 0;
                    tap_t = now_ms();
                    tap_x = f->x;
                    tap_y = f->y;
                }
            }
            last_x = f->x;
            last_y = f->y;
            if (npad > tap_fingers) {
                tap_fingers = npad;
            }
            if (abs(f->x - tap_x) > TAP_MAX_PX || abs(f->y - tap_y) > TAP_MAX_PX) {
                tap_moved = true;
            }
        } else {
            if (tap_on && !tap_moved && now_ms() - tap_t < TAP_MAX_MS) {
                click = tap_fingers >= 2 ? 2 : 1;
            }
            tap_on = false;
            pad_id = -1;
        }

        int dx = acc_x / 100, dy = acc_y / 100;
        acc_x -= dx * 100;
        acc_y -= dy * 100;
        if (dx || dy || want != buttons) {
            buttons = want;
            ps2_mouse_event(mouse, dx, dy, 0, buttons);
        }
        if (click) {
            ps2_mouse_event(mouse, 0, 0, 0, buttons | click);
            ps2_mouse_event(mouse, 0, 0, 0, buttons);
        }
    }
}

void touch_start(void)
{
    xTaskCreatePinnedToCore(touch_task, "touch", 3072, NULL, 2, NULL, 0);
}
