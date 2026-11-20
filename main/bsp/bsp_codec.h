/*
 * Codec hooks used by bsp_audio.c; each board's codec file implements them
 * (bsp_codec_es8311.c, bsp_codec_es8388.c). The codec is an I2S slave with
 * MCLK = 256 fs; bsp_audio.c owns I2S and keeps the amp off while it is set up.
 */
#pragma once
#include "bsp_board.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t bsp_codec_open(i2c_master_bus_handle_t bus, uint32_t sample_rate);
esp_err_t bsp_codec_set_rate(uint32_t sample_rate);
void bsp_codec_mute(bool mute);
void bsp_codec_set_volume(int percent);     /* 0..100, 0 = mute */

#ifdef __cplusplus
}
#endif
