/*
 * MIPI-DSI display bring-up for the M5Stack Tab5 (720x1280 portrait).
 *
 * The Tab5 has shipped with three panels. Like M5Stack's own BSP, we tell them
 * apart by the touch controller, which comes up first:
 *   GT911 at 0x14/0x5D           -> ILI9881C
 *   0x55, firmware version 1     -> ST7121
 *   0x55, other firmware version -> ST7123
 * If no touch answers, the ILI9881C ID (page 1, regs 00/01 = 98 81) is read
 * over DSI; failing that, ST7123 (the panel in current production) is assumed.
 *
 * Panel power and LCD_RST are handled by the I/O expander sequence in
 * bsp_power.c (run by bsp_i2c_init). The steps before and after each init table
 * follow what M5Stack's drivers do (esp_lcd_ili9881c, esp_lcd_st7123,
 * esp_lcd_st7121). Timings and lane rates are M5GFX's (specs::tab5): the older
 * M5Tab5-UserDemo values (ILI9881C at 60 MHz = ~48 Hz refresh) flickered.
 */
#include "bsp_board.h"
#if defined(BSP_BOARD_M5STACK_TAB5)
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_commands.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_display";

typedef struct {
    uint8_t cmd;
    const uint8_t *data;
    uint8_t len;
    uint16_t delay_ms;
} lcd_init_cmd_t;

#include "bsp_tab5_panel_init.h"

static bsp_panel_kind_t s_panel_kind = BSP_PANEL_UNKNOWN;
static esp_lcd_panel_handle_t s_panel = NULL;

bsp_panel_kind_t bsp_display_panel_kind(void) { return s_panel_kind; }

const char *bsp_display_panel_name(void)
{
    switch (s_panel_kind) {
    case BSP_PANEL_ILI9881C: return "ILI9881C";
    case BSP_PANEL_ST7123:   return "ST7123";
    case BSP_PANEL_ST7121:   return "ST7121";
    default:                 return "unknown";
    }
}

/* Identify the panel from its touch controller (see top of file). */
static bsp_panel_kind_t probe_by_touch(void)
{
    i2c_master_bus_handle_t bus = NULL;
    if (bsp_i2c_init(&bus) != ESP_OK) {
        return BSP_PANEL_UNKNOWN;
    }
    for (int tries = 0; tries < 30; tries++) {          /* up to ~0.6 s after power-up */
        if (i2c_master_probe(bus, 0x14, 20) == ESP_OK || i2c_master_probe(bus, 0x5D, 20) == ESP_OK) {
            ESP_LOGI(TAG, "GT911 touch -> ILI9881C panel");
            bsp_diag_note(BSP_DIAG_PANEL, "GT911 touch answered (try %d)", tries + 1);
            return BSP_PANEL_ILI9881C;
        }
        if (i2c_master_probe(bus, 0x55, 20) == ESP_OK) {
            i2c_master_dev_handle_t dev = NULL;
            i2c_device_config_t cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = 0x55,
                .scl_speed_hz = 100000,
            };
            uint8_t fw = 0;
            if (i2c_master_bus_add_device(bus, &cfg, &dev) == ESP_OK) {
                uint8_t reg[2] = {0x00, 0x00};
                i2c_master_transmit_receive(dev, reg, 2, &fw, 1, 50);
                i2c_master_bus_rm_device(dev);
            }
            ESP_LOGI(TAG, "ST712x touch, fw %u -> %s panel", fw, fw == 1 ? "ST7121" : "ST7123");
            bsp_diag_note(BSP_DIAG_PANEL, "touch 0x55 answered (try %d), fw %u", tries + 1, fw);
            return fw == 1 ? BSP_PANEL_ST7121 : BSP_PANEL_ST7123;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return BSP_PANEL_UNKNOWN;
}

static bool ili9881c_id_ok(esp_lcd_panel_io_handle_t io, uint8_t id[2])
{
    esp_lcd_panel_io_tx_param(io, 0xFF, (uint8_t[]){0x98, 0x81, 0x01}, 3);
    esp_lcd_panel_io_rx_param(io, 0x00, &id[0], 1);
    esp_lcd_panel_io_rx_param(io, 0x01, &id[1], 1);
    esp_lcd_panel_io_tx_param(io, 0xFF, (uint8_t[]){0x98, 0x81, 0x00}, 3);
    ESP_LOGI(TAG, "ILI9881C ID: %02X %02X", id[0], id[1]);
    return id[0] == 0x98 && id[1] == 0x81;
}

static esp_err_t send_init_cmds(esp_lcd_panel_io_handle_t io, const lcd_init_cmd_t *cmds, size_t n,
                                uint16_t extra_delay_ms)
{
    for (size_t i = 0; i < n; i++) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, cmds[i].cmd, cmds[i].len ? cmds[i].data : NULL,
                                                      cmds[i].len),
                            TAG, "send cmd 0x%02X failed", cmds[i].cmd);
        uint16_t d = cmds[i].delay_ms + extra_delay_ms;
        if (d) {
            vTaskDelay(pdMS_TO_TICKS(d));
        }
    }
    return ESP_OK;
}

#define N(a) (sizeof(a) / sizeof((a)[0]))

static uint32_t lane_mbps_for(bsp_panel_kind_t kind)
{
    return kind == BSP_PANEL_ST7121 ? 900 : 1040;
}

static esp_err_t open_dsi(bsp_panel_kind_t kind, esp_lcd_dsi_bus_handle_t *bus, esp_lcd_panel_io_handle_t *io)
{
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = BSP_LCD_DSI_LANES,
#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3
        /* Pre-v3 silicon: _DEFAULT is XTAL, which only v3+ supports -> abort() */
        .phy_clk_src = MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT_LEGACY,
#else
        .phy_clk_src = MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT,
#endif
        .lane_bit_rate_mbps = lane_mbps_for(kind),
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, bus), TAG, "DSI bus failed");
    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_err_t err = esp_lcd_new_panel_io_dbi(*bus, &dbi_cfg, io);
    if (err != ESP_OK) {
        esp_lcd_del_dsi_bus(*bus);
        *bus = NULL;
    }
    return err;
}

esp_err_t bsp_display_new(esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(ret_panel, ESP_ERR_INVALID_ARG, TAG, "null arg");
    if (s_panel) {
        *ret_panel = s_panel;
        return ESP_OK;
    }

    /* 0. Expander power-up (LCD/touch out of reset), then identify the panel */
    s_panel_kind = probe_by_touch();
    bool guessed = s_panel_kind == BSP_PANEL_UNKNOWN;
    if (guessed) {
        ESP_LOGW(TAG, "no touch controller answered; checking for an ILI9881C over DSI");
        s_panel_kind = BSP_PANEL_ILI9881C;
    }

    /* 1. Power the MIPI D-PHY */
    static esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = BSP_MIPI_DPHY_LDO_CHAN,
        .voltage_mv = BSP_MIPI_DPHY_LDO_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo), TAG, "LDO acquire failed");

    /* 2. DSI bus + DBI command interface (lane rate depends on the panel) */
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(open_dsi(s_panel_kind, &dsi_bus, &io), TAG, "DSI");
    if (guessed) {
        uint8_t id[2] = {0};
        if (ili9881c_id_ok(io, id)) {
            bsp_diag_note(BSP_DIAG_PANEL, "no touch answered; ILI9881C ID 98 81 read over DSI");
        } else {
            bsp_diag_note(BSP_DIAG_PANEL, "no touch answered; ILI ID %02X %02X -> assuming ST7123",
                          id[0], id[1]);
            s_panel_kind = BSP_PANEL_ST7123;
            esp_lcd_panel_io_del(io);
            esp_lcd_del_dsi_bus(dsi_bus);
            ESP_RETURN_ON_ERROR(open_dsi(s_panel_kind, &dsi_bus, &io), TAG, "DSI");
        }
    }

    /* 3. Per-panel video timing (M5GFX specs::tab5) */
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = BSP_LCD_H_RES,
            .v_size = BSP_LCD_V_RES,
        },
    };
    switch (s_panel_kind) {
    case BSP_PANEL_ST7123:
        dpi_cfg.dpi_clock_freq_mhz = 80;
        dpi_cfg.video_timing.hsync_pulse_width = 2;
        dpi_cfg.video_timing.hsync_back_porch = 40;
        dpi_cfg.video_timing.hsync_front_porch = 40;
        dpi_cfg.video_timing.vsync_pulse_width = 2;     /* back + pulse must stay 10 (M5 note) */
        dpi_cfg.video_timing.vsync_back_porch = 8;
        dpi_cfg.video_timing.vsync_front_porch = 220;   /* shorter stops the touch (M5 note) */
        break;
    case BSP_PANEL_ST7121:
        dpi_cfg.dpi_clock_freq_mhz = 70;
        dpi_cfg.video_timing.hsync_pulse_width = 2;
        dpi_cfg.video_timing.hsync_back_porch = 40;
        dpi_cfg.video_timing.hsync_front_porch = 40;
        dpi_cfg.video_timing.vsync_pulse_width = 20;
        dpi_cfg.video_timing.vsync_back_porch = 24;
        dpi_cfg.video_timing.vsync_front_porch = 200;
        break;
    default: /* ILI9881C */
        dpi_cfg.dpi_clock_freq_mhz = 80;
        dpi_cfg.video_timing.hsync_pulse_width = 40;
        dpi_cfg.video_timing.hsync_back_porch = 140;
        dpi_cfg.video_timing.hsync_front_porch = 40;
        dpi_cfg.video_timing.vsync_pulse_width = 4;
        dpi_cfg.video_timing.vsync_back_porch = 20;
        dpi_cfg.video_timing.vsync_front_porch = 20;
        break;
    }

    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_dpi(dsi_bus, &dpi_cfg, &panel), TAG, "DPI panel failed");

    /* 4. Vendor init over DBI (LP mode), then start the video stream */
    ESP_LOGI(TAG, "Initialising %s", bsp_display_panel_name());
    switch (s_panel_kind) {
    case BSP_PANEL_ST7123: {
        vTaskDelay(pdMS_TO_TICKS(20));                 /* driver's "reset" without a GPIO */
        uint8_t id[3] = {0};
        esp_lcd_panel_io_rx_param(io, 0x04, id, 3);
        ESP_LOGI(TAG, "ST7123 DCS ID %02X %02X %02X", id[0], id[1], id[2]);
        ESP_RETURN_ON_ERROR(send_init_cmds(io, st7123_init_cmds, N(st7123_init_cmds), 0), TAG, "ST7123 init");
        break;
    }
    case BSP_PANEL_ST7121:
        esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(120));
        ESP_RETURN_ON_ERROR(send_init_cmds(io, st7121_init_cmds, N(st7121_init_cmds), 5), TAG, "ST7121 init");
        break;
    default:
        esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        /* esp_lcd_ili9881c: 2-lane pad control on page 1, sleep out, MADCTL/COLMOD on page 0 */
        esp_lcd_panel_io_tx_param(io, 0xFF, (uint8_t[]){0x98, 0x81, 0x01}, 3);
        esp_lcd_panel_io_tx_param(io, 0xB7, (uint8_t[]){0x03}, 1);
        esp_lcd_panel_io_tx_param(io, 0xFF, (uint8_t[]){0x98, 0x81, 0x00}, 3);
        esp_lcd_panel_io_tx_param(io, LCD_CMD_SLPOUT, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(120));
        esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]){0x00}, 1);
        esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD, (uint8_t[]){0x55}, 1);   /* 16 bpp */
        ESP_RETURN_ON_ERROR(send_init_cmds(io, ili9881c_init_cmds, N(ili9881c_init_cmds), 0), TAG, "ILI9881C init");
        break;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "DPI start failed");
    esp_lcd_panel_io_tx_param(io, LCD_CMD_DISPON, NULL, 0);

    s_panel = panel;
    *ret_panel = panel;
    return ESP_OK;
}

#endif /* BSP_BOARD_M5STACK_TAB5 */
