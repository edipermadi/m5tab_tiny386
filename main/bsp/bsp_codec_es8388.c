/*
 * ES8388 speaker codec (M5Stack Tab5): I2C 0x10 on the SYS bus, I2S slave,
 * 16-bit Philips, MCLK = 256 fs (single speed). In slave mode the ES8388 takes
 * its rate from LRCK at a fixed MCLK/LRCK ratio, so a rate change needs no
 * register writes.
 *
 * The register sequence is Espressif's esp_codec_dev ES8388 driver (es8388_open
 * + set_fs + start for the DAC path, Apache-2.0), as M5Stack's Tab5 BSP uses it.
 * The Tab5's speaker amp runs from 5 V and the DAC from 3.3 V, so M5 caps the
 * DAC about 3.6 dB below full scale (esp_codec_dev hw_gain); we do the same.
 */
#include "bsp_codec.h"
#if BSP_AUDIO_CODEC_ES8388
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "es8388";

#define ES8388_ADDR        0x10      /* CE low (esp_codec_dev's 0x20 is the 8-bit form) */
#define ES8388_I2C_HZ      100000

/* registers */
#define ES8388_CONTROL1     0x00
#define ES8388_CONTROL2     0x01
#define ES8388_CHIPPOWER    0x02
#define ES8388_ADCPOWER     0x03
#define ES8388_DACPOWER     0x04
#define ES8388_MASTERMODE   0x08
#define ES8388_ADCCONTROL1  0x09
#define ES8388_ADCCONTROL2  0x0a
#define ES8388_ADCCONTROL3  0x0b
#define ES8388_ADCCONTROL4  0x0c
#define ES8388_ADCCONTROL5  0x0d
#define ES8388_ADCCONTROL8  0x10
#define ES8388_ADCCONTROL9  0x11
#define ES8388_DACCONTROL1  0x17
#define ES8388_DACCONTROL2  0x18
#define ES8388_DACCONTROL3  0x19
#define ES8388_DACCONTROL4  0x1a
#define ES8388_DACCONTROL5  0x1b
#define ES8388_DACCONTROL16 0x26
#define ES8388_DACCONTROL17 0x27
#define ES8388_DACCONTROL20 0x2a
#define ES8388_DACCONTROL21 0x2b
#define ES8388_DACCONTROL23 0x2d
#define ES8388_DACCONTROL24 0x2e
#define ES8388_DACCONTROL25 0x2f
#define ES8388_DACCONTROL26 0x30
#define ES8388_DACCONTROL27 0x31

/* DAC digital volume: 0x00 = 0 dB ... 0xC0 = -96 dB, 0.5 dB per step */
#define DAC_VOL_MAX_STEP   7         /* -3.5 dB at 100 % (5 V amp headroom, see above) */
#define DAC_VOL_MUTE       0xC0

static i2c_master_dev_handle_t s_codec = NULL;

static esp_err_t es_wr(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_codec, buf, sizeof(buf), 100);
}

static esp_err_t es_rd(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_codec, &reg, 1, val, 1, 100);
}

static esp_err_t es8388_init(void)
{
    uint8_t probe = 0;
    ESP_RETURN_ON_ERROR(es_rd(ES8388_DACCONTROL21, &probe), TAG, "ES8388 not responding at 0x%02x", ES8388_ADDR);

    esp_err_t r = ESP_OK;
    /* es8388_open */
    r |= es_wr(ES8388_DACCONTROL3, 0x04);      /* mute */
    r |= es_wr(ES8388_CONTROL2, 0x50);
    r |= es_wr(ES8388_CHIPPOWER, 0x00);        /* power up all */
    r |= es_wr(0x35, 0xA0);                    /* internal DLL off (better at 8 kHz) */
    r |= es_wr(0x37, 0xD0);
    r |= es_wr(0x39, 0xD0);
    r |= es_wr(ES8388_MASTERMODE, 0x00);       /* I2S slave */
    r |= es_wr(ES8388_DACPOWER, 0xC0);         /* DAC + outputs off while configuring */
    r |= es_wr(ES8388_CONTROL1, 0x12);
    r |= es_wr(ES8388_DACCONTROL1, 0x18);      /* 16-bit I2S */
    r |= es_wr(ES8388_DACCONTROL2, 0x02);      /* single speed, MCLK/LRCK = 256 */
    r |= es_wr(ES8388_DACCONTROL16, 0x00);
    r |= es_wr(ES8388_DACCONTROL17, 0x90);     /* left DAC -> left mixer, 0 dB */
    r |= es_wr(ES8388_DACCONTROL20, 0x90);     /* right DAC -> right mixer, 0 dB */
    r |= es_wr(ES8388_DACCONTROL21, 0x80);     /* ADC and DAC share LRCK */
    r |= es_wr(ES8388_DACCONTROL23, 0x00);
    r |= es_wr(ES8388_DACCONTROL5, 0x00);      /* DAC volume 0 dB (set properly below) */
    r |= es_wr(ES8388_DACCONTROL4, 0x00);
    r |= es_wr(ES8388_DACCONTROL24, 0x1E);     /* LOUT1/ROUT1 0 dB */
    r |= es_wr(ES8388_DACCONTROL25, 0x1E);
    r |= es_wr(ES8388_DACCONTROL26, 0x00);     /* LOUT2/ROUT2 */
    r |= es_wr(ES8388_DACCONTROL27, 0x00);
    r |= es_wr(ES8388_DACPOWER, 0x3C);         /* DAC + LOUT1/ROUT1/LOUT2/ROUT2 on */
    /* ADC block exactly as esp_codec_dev leaves it (the mics are not used here) */
    r |= es_wr(ES8388_ADCPOWER, 0xFF);
    r |= es_wr(ES8388_ADCCONTROL1, 0xBB);
    r |= es_wr(ES8388_ADCCONTROL2, 0x00);
    r |= es_wr(ES8388_ADCCONTROL3, 0x02);
    r |= es_wr(ES8388_ADCCONTROL4, 0x0C);
    r |= es_wr(ES8388_ADCCONTROL5, 0x02);
    r |= es_wr(ES8388_ADCCONTROL8, 0x00);
    r |= es_wr(ES8388_ADCCONTROL9, 0x00);
    r |= es_wr(ES8388_ADCPOWER, 0x09);
    /* set_fs: normal I2S, 16 bit (already in DACCONTROL1) */
    /* start (DAC) */
    r |= es_wr(ES8388_DACCONTROL21, 0x80);
    r |= es_wr(ES8388_DACPOWER, 0x3C);
    r |= es_wr(ES8388_DACCONTROL3, 0x00);      /* unmute */

    ESP_RETURN_ON_FALSE(r == ESP_OK, ESP_FAIL, TAG, "ES8388 register setup failed");
    ESP_LOGI(TAG, "ES8388 ready");
    return ESP_OK;
}

esp_err_t bsp_codec_open(i2c_master_bus_handle_t bus, uint32_t sample_rate)
{
    (void)sample_rate;
    if (!s_codec) {
        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = ES8388_ADDR,
            .scl_speed_hz = ES8388_I2C_HZ,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev, &s_codec), TAG, "add ES8388 failed");
    }
    return es8388_init();
}

esp_err_t bsp_codec_set_rate(uint32_t sample_rate)
{
    (void)sample_rate;                         /* follows LRCK, see top of file */
    return ESP_OK;
}

void bsp_codec_mute(bool mute)
{
    if (!s_codec) {
        return;
    }
    uint8_t v = 0;
    if (es_rd(ES8388_DACCONTROL3, &v) == ESP_OK) {
        es_wr(ES8388_DACCONTROL3, (v & 0xFB) | (mute ? 0x04 : 0x00));
    }
}

void bsp_codec_set_volume(int percent)
{
    if (!s_codec) {
        return;
    }
    /* Same curve as the ES8311 board: 1..100 % -> -47.5..0 dB (here offset by the headroom). */
    uint8_t reg = percent <= 0 ? DAC_VOL_MUTE : (uint8_t)(DAC_VOL_MAX_STEP + (100 - percent) * 95 / 100);
    es_wr(ES8388_DACCONTROL5, reg);
    es_wr(ES8388_DACCONTROL4, reg);
}

#endif /* BSP_AUDIO_CODEC_ES8388 */
