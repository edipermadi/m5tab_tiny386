/*
 * M5Stack Tab5 Keyboard (A164) -> emulated PS/2 keyboard.
 *
 * The keyboard is an STM32 on I2C (address 0x6D, SDA G0 / SCL G1 on the Tab5).
 * It is used in its "Normal" mode: register 0x20 pops one event from a queue,
 * [7] = pressed, [6:4] = row (0-4), [3:0] = column (0-13); 0xFF = queue empty.
 * (Register map from M5Stack's M5Unit-KEYBOARD library, unit_Tab5Keyboard.)
 *
 * Layout (5 x 14):
 *   0: Esc  1  2  3  4  5  6  7  8  9  0  -  +  Del
 *   1: `    !  @  #  $  %  ^  &  *  (  )  [  ]  \
 *   2: Tab  q  w  e  r  t  y  u  i  o  p  ;  '  Backspace
 *   3: Sym  Aa a  s  d  f  g  h  j  k  l  Up _  Enter
 *   4: Ctrl Alt z x  c  v  b  n  m  .  Left Down Right Space
 *
 * PC mapping: Aa = Shift, Ctrl/Alt as such. Keys whose legend is a shifted
 * character (! @ # ... + _) send Shift with the key. Sym is a function layer:
 *   Sym + 1..0 - +       F1..F12
 *   Sym + Up/Down        Page Up / Page Down
 *   Sym + Left/Right     Home / End
 *   Sym + Del            Insert
 *   Sym + Esc            ` (grave)
 *   Sym + Tab            Caps Lock
 *   Sym + Alt            Windows key
 *   Sym + Backspace      Delete
 *   Sym + symbols        the alternate legends: ~ ? / < > { } | : " = ,
 * A key is released exactly as it was pressed, even if Sym or Aa changed meanwhile.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"

#include "tab5.h"
#include "common.h"
#include "../tiny386/i8042.h"

#if CONFIG_TAB5_KBD_ENABLE

static const char *TAG = "tab5kbd";

#define REG_INT_STAT     0x01
#define REG_EVENT_NUM    0x02
#define REG_MODE_KBD     0x10
#define REG_KEY_EVENT    0x20
#define REG_FW_VERSION   0xFE
#define EVENT_EMPTY      0xFF

#define ROWS 5
#define COLS 14

/* Linux input keycodes (what tiny386's ps2_put_keycode() takes) */
enum {
    K_ESC = 1, K_1, K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9, K_0, K_MINUS, K_EQUAL,
    K_BACKSPACE, K_TAB, K_Q, K_W, K_E, K_R, K_T, K_Y, K_U, K_I, K_O, K_P,
    K_LEFTBRACE, K_RIGHTBRACE, K_ENTER, K_LEFTCTRL, K_A, K_S, K_D, K_F, K_G, K_H,
    K_J, K_K, K_L, K_SEMICOLON, K_APOSTROPHE, K_GRAVE, K_LEFTSHIFT, K_BACKSLASH,
    K_Z, K_X, K_C, K_V, K_B, K_N, K_M, K_COMMA, K_DOT, K_SLASH, K_RIGHTSHIFT,
    K_KPASTERISK, K_LEFTALT, K_SPACE, K_CAPSLOCK,
    K_F1, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9, K_F10,
    K_F11 = 87, K_F12 = 88,
    K_HOME = 102, K_UP = 103, K_PAGEUP = 104, K_LEFT = 105, K_RIGHT = 106,
    K_END = 107, K_DOWN = 108, K_PAGEDOWN = 109, K_INSERT = 110, K_DELETE = 111,
    K_LEFTMETA = 125,
};

#define SH 0x100        /* the key is sent with Shift */
#define NOSH 0x200      /* the key is sent without Shift, even if Aa is held */
#define KC(v) ((v) & 0xFF)

/* special positions */
#define SYM_R 3
#define SYM_C 0
#define AA_R 3
#define AA_C 1
#define CTRL_R 4
#define CTRL_C 0
#define ALT_R 4
#define ALT_C 1

static const uint16_t base_map[ROWS][COLS] = {
    { K_ESC, K_1, K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9, K_0, K_MINUS, SH | K_EQUAL, K_DELETE },
    { K_GRAVE, SH | K_1, SH | K_2, SH | K_3, SH | K_4, SH | K_5, SH | K_6, SH | K_7, SH | K_8,
      SH | K_9, SH | K_0, K_LEFTBRACE, K_RIGHTBRACE, K_BACKSLASH },
    { K_TAB, K_Q, K_W, K_E, K_R, K_T, K_Y, K_U, K_I, K_O, K_P, K_SEMICOLON, K_APOSTROPHE, K_BACKSPACE },
    { 0, K_LEFTSHIFT, K_A, K_S, K_D, K_F, K_G, K_H, K_J, K_K, K_L, K_UP, SH | K_MINUS, K_ENTER },
    { K_LEFTCTRL, K_LEFTALT, K_Z, K_X, K_C, K_V, K_B, K_N, K_M, K_DOT, K_LEFT, K_DOWN, K_RIGHT, K_SPACE },
};

static const uint16_t sym_map[ROWS][COLS] = {
    { K_GRAVE, K_F1, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9, K_F10, K_F11, K_F12, K_INSERT },
    { SH | K_GRAVE, SH | K_SLASH, SH | K_2, SH | K_3, SH | K_4, SH | K_5, SH | K_6, SH | K_7,
      NOSH | K_SLASH, SH | K_COMMA, SH | K_DOT, SH | K_LEFTBRACE, SH | K_RIGHTBRACE, SH | K_BACKSLASH },
    { K_CAPSLOCK, K_Q, K_W, K_E, K_R, K_T, K_Y, K_U, K_I, K_O, K_P, SH | K_SEMICOLON,
      SH | K_APOSTROPHE, K_DELETE },
    { 0, K_LEFTSHIFT, K_A, K_S, K_D, K_F, K_G, K_H, K_J, K_K, K_L, K_PAGEUP, NOSH | K_EQUAL, K_ENTER },
    { K_LEFTCTRL, K_LEFTMETA, K_Z, K_X, K_C, K_V, K_B, K_N, K_M, NOSH | K_COMMA, K_HOME,
      K_PAGEDOWN, K_END, K_SPACE },
};

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

static bool s_sym;                     /* Sym held */
static int s_shift_held;               /* real shift keys (Aa) held */
static uint16_t s_sent[ROWS][COLS];    /* what each pressed key sent, for its release */

static esp_err_t reg_read(uint8_t reg, uint8_t *val)
{
    /* register write, STOP, then read (as M5UnitUnified does) */
    esp_err_t err = i2c_master_transmit(s_dev, &reg, 1, 50);
    if (err == ESP_OK) {
        err = i2c_master_receive(s_dev, val, 1, 50);
    }
    return err;
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, 2, 50);
}

static void put_key(bool down, int code)
{
    if (globals.kbd && code) {
        ps2_put_keycode(globals.kbd, down, code);
    }
}

static void key_event(int row, int col, bool pressed)
{
    if (row >= ROWS || col >= COLS) {
        return;
    }
    if (row == SYM_R && col == SYM_C) {
        s_sym = pressed;
        return;
    }

    if (pressed) {
        uint16_t m = s_sym ? sym_map[row][col] : base_map[row][col];
        if (!m) {
            return;
        }
        s_sent[row][col] = m;
        int code = KC(m);
        if (code == K_LEFTSHIFT) {
            s_shift_held++;
        }
        if ((m & SH) && !s_shift_held) {
            put_key(true, K_LEFTSHIFT);
        } else if ((m & NOSH) && s_shift_held) {
            put_key(false, K_LEFTSHIFT);
        }
        put_key(true, code);
    } else {
        uint16_t m = s_sent[row][col];
        if (!m) {
            return;
        }
        s_sent[row][col] = 0;
        int code = KC(m);
        put_key(false, code);
        if (code == K_LEFTSHIFT && s_shift_held > 0) {
            s_shift_held--;
        }
        if ((m & SH) && !s_shift_held) {
            put_key(false, K_LEFTSHIFT);
        } else if ((m & NOSH) && s_shift_held) {
            put_key(true, K_LEFTSHIFT);
        }
    }
}

static void kbd_task(void *arg)
{
    /* wait for the emulated PC (globals.kbd) */
    xEventGroupWaitBits(global_event_group, BIT0, pdFALSE, pdFALSE, portMAX_DELAY);

    bool present = false;
    for (;;) {
        if (!present) {
            uint8_t fw = 0;
            if (reg_read(REG_FW_VERSION, &fw) == ESP_OK &&
                reg_write(REG_MODE_KBD, 0) == ESP_OK) {      /* Normal mode, clears the queue */
                reg_write(REG_EVENT_NUM, 0);
                memset(s_sent, 0, sizeof(s_sent));
                s_sym = false;
                s_shift_held = 0;
                present = true;
                ESP_LOGI(TAG, "Tab5 Keyboard found, firmware 0x%02X", fw);
            } else {
                vTaskDelay(pdMS_TO_TICKS(1000));             /* not attached (yet) */
                continue;
            }
        }

        /* drain the event queue */
        for (int n = 0; n < 32; n++) {
            uint8_t ev = EVENT_EMPTY;
            if (reg_read(REG_KEY_EVENT, &ev) != ESP_OK) {
                ESP_LOGW(TAG, "Tab5 Keyboard lost");
                present = false;
                break;
            }
            if (ev == EVENT_EMPTY) {
                break;
            }
            key_event((ev >> 4) & 0x07, ev & 0x0F, (ev & 0x80) != 0);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void tab5_kbd_start(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = CONFIG_TAB5_KBD_I2C_PORT,
        .sda_io_num = CONFIG_TAB5_KBD_SDA_GPIO,
        .scl_io_num = CONFIG_TAB5_KBD_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        return;
    }
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_TAB5_KBD_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(s_bus, &dev, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(err));
        return;
    }
    xTaskCreatePinnedToCore(kbd_task, "tab5_kbd", 3072, NULL, 2, NULL, 0);
}

#else
void tab5_kbd_start(void) {}
#endif
