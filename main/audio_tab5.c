/*
 * Sound: tiny386's mixer (PC speaker, SB16, AdLib) at 44.1 kHz stereo 16-bit
 * -> BSP I2S0 -> ES8388 -> speaker amp.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "bsp/bsp_board.h"
#include "tab5.h"
#include "common.h"
#include "../tiny386/pc.h"

static const char *TAG = "audio";

static void audio_task(void *arg)
{
    static int16_t buf[MIXER_BUF_LEN];     /* MIXER_BUF_LEN samples = MIXER_BUF_LEN / 2 frames */

    xEventGroupWaitBits(global_event_group, BIT0, pdFALSE, pdFALSE, portMAX_DELAY);
    ESP_LOGI(TAG, "audio runs on core %d", esp_cpu_get_core_id());

    for (;;) {
        memset(buf, 0, sizeof(buf));
        mixer_callback(globals.pc, (uint8_t *)buf, sizeof(buf));
        for (int i = 0; i < MIXER_BUF_LEN; i++) {
            buf[i] = buf[i] / 16;          /* same attenuation as tiny386's i2s.c */
        }
        bsp_audio_write(buf, MIXER_BUF_LEN / 2, portMAX_DELAY);
    }
}

void audio_start(void)
{
    esp_err_t err = bsp_audio_init(44100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "speaker init failed: %s", esp_err_to_name(err));
        return;
    }
    bsp_audio_set_volume(80);
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 1, NULL, 0);
}
