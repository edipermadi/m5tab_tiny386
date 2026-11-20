/*
 * M5Stack Tab5 board support, trimmed from the esp32p4_86box BSP
 * (Tab5 only: power/I/O expanders, MIPI-DSI panel, backlight, ES8388 speaker).
 *
 *  - 5" 720x1280 MIPI-DSI (ILI9881C, ST7123 or ST7121, detected at boot)
 *  - ES8388 speaker codec + amp
 *  - two PI4IOE5V6408 I/O expanders for the power/reset lines
 * Pins/timings from M5Stack's M5Tab5-UserDemo BSP and M5GFX.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_BOARD_M5STACK_TAB5   1
#define BSP_BOARD_NAME           "M5Stack Tab5"

#define BSP_LCD_H_RES            720
#define BSP_LCD_V_RES            1280
#define BSP_LCD_BITS_PER_PIXEL   16      /* RGB565 */
#define BSP_LCD_DSI_LANES        2
#define BSP_LCD_BACKLIGHT_GPIO   22      /* LEDC PWM, active high */
#define BSP_MIPI_DPHY_LDO_CHAN   3
#define BSP_MIPI_DPHY_LDO_MV     2500
#define BSP_TOUCH_INT_GPIO       23      /* held high while the touch leaves reset -> GT911 at 0x14 */

#define BSP_I2C_PORT             1       /* "SYS" I2C: expanders, touch, codecs */
#define BSP_I2C_SDA_GPIO         31
#define BSP_I2C_SCL_GPIO         32

#define BSP_AUDIO_CODEC_ES8388   1
#define BSP_I2S_MCLK_GPIO        30
#define BSP_I2S_BCLK_GPIO        27
#define BSP_I2S_WS_GPIO          29
#define BSP_I2S_DOUT_GPIO        26      /* SoC -> ES8388 DSDIN */
/* speaker amp enable is I/O expander 1, P1 */

/* microSD: SDMMC slot 0 (IO_MUX): CLK 43, CMD 44, D0..D3 = 39..42, IO rail from LDO VO4 */
#define BSP_SD_LDO_CHAN          4

typedef enum {
    BSP_PANEL_UNKNOWN = 0,
    BSP_PANEL_ILI9881C,     /* GT911 touch */
    BSP_PANEL_ST7123,       /* touch in the panel controller */
    BSP_PANEL_ST7121,       /* touch in the panel controller */
} bsp_panel_kind_t;

/* Shared SYS I2C bus. Created on first call; also runs the expander power-up
 * sequence (LCD/touch reset, EXT5V, USB-A 5V, Wi-Fi power, charger). */
esp_err_t bsp_i2c_init(i2c_master_bus_handle_t *ret_bus);

/* Speaker amplifier on/off (I/O expander). */
void bsp_audio_amp_set(bool on);

enum { BSP_DIAG_POWER, BSP_DIAG_PANEL, BSP_DIAG_TOUCH, BSP_DIAG_SLOTS };
void bsp_diag_note(int slot, const char *fmt, ...);

/* Powers the D-PHY, brings up the DSI link, detects which panel is fitted and
 * initialises it (one PSRAM frame buffer). Backlight is left off. */
esp_err_t bsp_display_new(esp_lcd_panel_handle_t *ret_panel);
bsp_panel_kind_t bsp_display_panel_kind(void);
const char *bsp_display_panel_name(void);

/* Backlight brightness 0..100 %. */
void bsp_display_brightness_set(int percent);

/* Speaker: I2S0 (TX only, 16-bit stereo) -> ES8388 -> amp. Supported rates:
 * 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000. */
esp_err_t bsp_audio_init(uint32_t sample_rate);
esp_err_t bsp_audio_set_sample_rate(uint32_t sample_rate);
bool bsp_audio_acquire(uint32_t timeout_ms);
void bsp_audio_release(void);
/* Blocking write of interleaved L/R 16-bit frames. */
esp_err_t bsp_audio_write(const int16_t *stereo_frames, size_t frames, uint32_t timeout_ms);
/* 0..100 % (0 = mute). */
void bsp_audio_set_volume(int percent);

#ifdef __cplusplus
}
#endif
