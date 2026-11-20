/*
 * Shared I2C bus + board power/reset lines.
 *
 * Waveshare 4B: plain GPIOs (speaker amp on GPIO53); the panel reset is driven
 * by bsp_display.c.
 *
 * M5Stack Tab5: two PI4IOE5V6408 8-bit I/O expanders on the SYS I2C bus.
 *   E1 @0x43: P0 RF antenna switch, P1 SPK_EN, P2 EXT5V_EN, P4 LCD_RST,
 *             P5 TP_RST, P6 CAM_RST, P7 HP_DET (input)
 *   E2 @0x44: P0 WLAN_PWR_EN (ESP32-C6), P3 USB5V_EN, P4 PWROFF_PULSE,
 *             P5 nCHG_QC_EN, P6 CHG_STAT (input), P7 CHG_EN
 * The register values and order are M5Stack's Tab5 power-on sequence (M5GFX
 * pmic_ops tab5_power_on / M5Tab5-UserDemo bsp_io_expander_pi4ioe_init).
 * TP_INT (GPIO23) is held high while the touch controller leaves reset, which
 * puts a GT911 at address 0x14. LCD_RST has no pull-up on the panel side, so it
 * is released by switching P4 to an input with the expander's pull-up rather
 * than by driving it high.
 */
#include "bsp_board.h"
#include <stdarg.h>
#include <stdio.h>
#include "esp_check.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_power";

static i2c_master_bus_handle_t s_bus = NULL;
static char s_diag[BSP_DIAG_SLOTS][96];

void bsp_diag_note(int slot, const char *fmt, ...)
{
    if (slot < 0 || slot >= BSP_DIAG_SLOTS) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_diag[slot], sizeof(s_diag[slot]), fmt, ap);
    va_end(ap);
}

#if defined(BSP_BOARD_M5STACK_TAB5)
#define PI4IO_ADDR_E1      0x43
#define PI4IO_ADDR_E2      0x44
#define PI4IO_REG_ID       0x01
#define PI4IO_REG_IO_DIR   0x03     /* 1 = output */
#define PI4IO_REG_OUT      0x05
#define PI4IO_REG_OUT_HIZ  0x07
#define PI4IO_REG_IN_DEF   0x09
#define PI4IO_REG_PULL_EN  0x0B
#define PI4IO_REG_PULL_SEL 0x0D     /* 1 = pull-up */
#define PI4IO_REG_IN       0x0F
#define PI4IO_REG_INT_MASK 0x11

#define E1_SPK_EN          (1u << 1)

static i2c_master_dev_handle_t s_e1, s_e2;
static uint8_t s_e1_out;            /* shadow of E1's output register */

static esp_err_t pi4io_wr(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(dev, b, 2, 50);
}

static esp_err_t tab5_power_init(void)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PI4IO_ADDR_E1,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &cfg, &s_e1), TAG, "add E1");
    cfg.device_address = PI4IO_ADDR_E2;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &cfg, &s_e2), TAG, "add E2");

    uint8_t reg = PI4IO_REG_ID, id = 0, id2 = 0;
    esp_err_t e = i2c_master_transmit_receive(s_e1, &reg, 1, &id, 1, 50);
    i2c_master_transmit_receive(s_e2, &reg, 1, &id2, 1, 50);
    if (e != ESP_OK) {
        bsp_diag_note(BSP_DIAG_POWER, "I/O expander 0x43 not responding: %s", esp_err_to_name(e));
        return e;
    }
    ESP_LOGI(TAG, "PI4IOE5V6408 id 0x%02x", id);

    /* TP_INT high while the touch controller comes out of reset */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BSP_TOUCH_INT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(BSP_TOUCH_INT_GPIO, 1);

    esp_err_t r = ESP_OK;
    /* E1: everything output except HP_DET; LCD_RST(P4) and TP_RST(P5) low = in reset.
     * Speaker amp stays off until the codec is set up (bsp_audio_amp_set). */
    r |= pi4io_wr(s_e1, PI4IO_REG_OUT, 0x44);        /* P2 EXT5V, P6 CAM_RST high */
    r |= pi4io_wr(s_e1, PI4IO_REG_IO_DIR, 0x7F);
    r |= pi4io_wr(s_e1, PI4IO_REG_OUT_HIZ, 0x00);
    r |= pi4io_wr(s_e1, PI4IO_REG_PULL_SEL, 0x7F);
    r |= pi4io_wr(s_e1, PI4IO_REG_PULL_EN, 0x7F);
    /* E2: WLAN_PWR_EN, USB5V_EN, CHG_EN on; CHG_STAT is an input */
    r |= pi4io_wr(s_e2, PI4IO_REG_IO_DIR, 0xB9);
    r |= pi4io_wr(s_e2, PI4IO_REG_OUT_HIZ, 0x06);
    r |= pi4io_wr(s_e2, PI4IO_REG_PULL_SEL, 0xB9);
    r |= pi4io_wr(s_e2, PI4IO_REG_PULL_EN, 0xF9);
    r |= pi4io_wr(s_e2, PI4IO_REG_IN_DEF, 0x40);
    r |= pi4io_wr(s_e2, PI4IO_REG_INT_MASK, 0xBF);
    r |= pi4io_wr(s_e2, PI4IO_REG_OUT, 0x89);
    vTaskDelay(pdMS_TO_TICKS(10));
    /* release LCD_RST (P4 -> input with pull-up; its latch stays 0) and TP_RST */
    r |= pi4io_wr(s_e1, PI4IO_REG_IO_DIR, 0x6F);
    s_e1_out = 0x64;                                 /* P2 EXT5V, P5 TP_RST, P6 CAM_RST */
    r |= pi4io_wr(s_e1, PI4IO_REG_OUT, s_e1_out);
    bsp_diag_note(BSP_DIAG_POWER, "expanders 0x43 id %02x, 0x44 id %02x, setup %s",
                  id, id2, r == ESP_OK ? "ok" : "FAILED");
    ESP_RETURN_ON_FALSE(r == ESP_OK, ESP_FAIL, TAG, "I/O expander setup failed");

    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_direction(BSP_TOUCH_INT_GPIO, GPIO_MODE_INPUT);
    vTaskDelay(pdMS_TO_TICKS(100));                  /* panel + touch boot */
    return ESP_OK;
}
#endif /* BSP_BOARD_M5STACK_TAB5 */

esp_err_t bsp_i2c_init(i2c_master_bus_handle_t *ret_bus)
{
    if (!s_bus) {
        i2c_master_bus_config_t cfg = {
            .i2c_port = BSP_I2C_PORT,
            .sda_io_num = BSP_I2C_SDA_GPIO,
            .scl_io_num = BSP_I2C_SCL_GPIO,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_bus), TAG, "I2C bus init failed");
#if defined(BSP_BOARD_M5STACK_TAB5)
        esp_err_t err = tab5_power_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Tab5 power-up failed: %s", esp_err_to_name(err));
        }
#endif
    }
    if (ret_bus) {
        *ret_bus = s_bus;
    }
    return ESP_OK;
}

void bsp_audio_amp_set(bool on)
{
#if defined(BSP_BOARD_WAVESHARE_4B)
    static bool ready = false;
    if (!ready) {
        gpio_config_t pa = {
            .pin_bit_mask = 1ULL << BSP_AUDIO_PA_GPIO,
            .mode = GPIO_MODE_OUTPUT,
        };
        ready = gpio_config(&pa) == ESP_OK;
    }
    gpio_set_level(BSP_AUDIO_PA_GPIO, on);
#elif defined(BSP_BOARD_M5STACK_TAB5)
    if (!s_e1) {
        return;
    }
    s_e1_out = on ? (s_e1_out | E1_SPK_EN) : (s_e1_out & ~E1_SPK_EN);
    pi4io_wr(s_e1, PI4IO_REG_OUT, s_e1_out);
#endif
}

void bsp_power_down(void)
{
#if defined(BSP_BOARD_M5STACK_TAB5)
    if (!s_e1 || !s_e2) {
        return;
    }
    /* E1: speaker amp, EXT5V (keyboard port) and the antenna switch off; LCD
     * (P4, back to an output), touch and camera held in reset */
    s_e1_out = 0x00;
    pi4io_wr(s_e1, PI4IO_REG_OUT, s_e1_out);
    pi4io_wr(s_e1, PI4IO_REG_IO_DIR, 0x7F);
    /* E2: ESP32-C6 (WLAN_PWR_EN) and USB-A 5V off; the charger stays enabled
     * so the battery still charges while the board sleeps */
    pi4io_wr(s_e2, PI4IO_REG_OUT, 0x80);
#endif
}
