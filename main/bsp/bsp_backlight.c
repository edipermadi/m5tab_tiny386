/* Backlight: LEDC PWM on GPIO22 (active high). */
#include "bsp_board.h"
#include "driver/ledc.h"
#include "driver/gpio.h"

#define BL_TIMER    LEDC_TIMER_1
#define BL_CHANNEL  LEDC_CHANNEL_1
#define BL_FREQ_HZ  5000
#define BL_RES      LEDC_TIMER_10_BIT

void bsp_display_brightness_set(int percent)
{
    static bool ready = false;
    if (!ready) {
        /* held low through deep sleep at the last shutdown (esp_main.c) */
        gpio_hold_dis(BSP_LCD_BACKLIGHT_GPIO);
        const ledc_timer_config_t t = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = BL_RES,
            .timer_num = BL_TIMER,
            .freq_hz = BL_FREQ_HZ,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        const ledc_channel_config_t ch = {
            .gpio_num = BSP_LCD_BACKLIGHT_GPIO,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = BL_CHANNEL,
            .timer_sel = BL_TIMER,
            .duty = 0,
        };
        if (ledc_timer_config(&t) != ESP_OK || ledc_channel_config(&ch) != ESP_OK) {
            return;
        }
        ready = true;
    }
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL, (uint32_t)percent * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL);
}
