/*
 * Speaker output: I2S0 (TX only, 16-bit stereo, MCLK = 256 fs) into the board's
 * codec (ES8311 on the 4B, ES8388 on the Tab5, see bsp_codec_*.c), followed by
 * a class-D amp switched by bsp_audio_amp_set().
 */
#include "bsp_codec.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "bsp_audio";

#define AUDIO_MCLK_MULT    I2S_MCLK_MULTIPLE_256

static i2s_chan_handle_t s_tx = NULL;
static bool s_codec_ok = false;
static SemaphoreHandle_t s_owner = NULL;   /* one audio producer at a time (the MP3 player) */
static uint32_t s_rate = 0;

/* Both codecs run as I2S slaves with MCLK = 256 fs, so the rate set is the same. */
static bool rate_supported(uint32_t r)
{
    static const uint32_t rates[] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 };
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        if (rates[i] == r) {
            return true;
        }
    }
    return false;
}

void bsp_audio_set_volume(int percent)
{
    if (s_codec_ok) {
        bsp_codec_set_volume(percent < 0 ? 0 : (percent > 100 ? 100 : percent));
    }
}

/* ------------------------------------------------------------------- I2S */
esp_err_t bsp_audio_init(uint32_t sample_rate)
{
    if (s_tx) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(rate_supported(sample_rate), ESP_ERR_NOT_SUPPORTED, TAG,
                        "unsupported sample rate %lu", (unsigned long)sample_rate);
    if (!s_owner) {
        s_owner = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_owner, ESP_ERR_NO_MEM, TAG, "mutex alloc failed");
    }

    /* Codec on the shared I2C bus (on the Tab5 this also powers the board up) */
    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(bsp_i2c_init(&bus), TAG, "I2C bus failed");

    /* Amp off while the codec comes up, to avoid pops */
    bsp_audio_amp_set(false);

    /* I2S first so MCLK is running when the codec is configured */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;          /* underrun -> silence, not a buzz */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "I2S channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK_GPIO,
            .bclk = BSP_I2S_BCLK_GPIO,
            .ws = BSP_I2S_WS_GPIO,
            .dout = BSP_I2S_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
        },
    };
    std_cfg.clk_cfg.mclk_multiple = AUDIO_MCLK_MULT;
    esp_err_t err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_tx);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S setup failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_tx);
        s_tx = NULL;
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(bsp_codec_open(bus, sample_rate), TAG, "codec init failed");
    s_codec_ok = true;
    bsp_audio_set_volume(70);

    s_rate = sample_rate;

    vTaskDelay(pdMS_TO_TICKS(20));
    bsp_audio_amp_set(true);
    ESP_LOGI(TAG, "speaker ready, %lu Hz", (unsigned long)sample_rate);
    return ESP_OK;
}

esp_err_t bsp_audio_write(const int16_t *stereo_frames, size_t frames, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_tx, ESP_ERR_INVALID_STATE, TAG, "audio not initialised");
    size_t written = 0;
    return i2s_channel_write(s_tx, stereo_frames, frames * 2 * sizeof(int16_t), &written, timeout_ms);
}

esp_err_t bsp_audio_set_sample_rate(uint32_t sample_rate)
{
    ESP_RETURN_ON_FALSE(s_tx, ESP_ERR_INVALID_STATE, TAG, "audio not initialised");
    if (sample_rate == s_rate) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(rate_supported(sample_rate), ESP_ERR_NOT_SUPPORTED, TAG,
                        "unsupported sample rate %lu", (unsigned long)sample_rate);

    bsp_codec_mute(true);                         /* DAC soft-mute during the switch */
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    clk.mclk_multiple = AUDIO_MCLK_MULT;
    esp_err_t err = i2s_channel_disable(s_tx);
    if (err == ESP_OK) {
        err = i2s_channel_reconfig_std_clock(s_tx, &clk);
    }
    esp_err_t en = i2s_channel_enable(s_tx);
    if (err == ESP_OK) {
        err = en;
    }
    if (err == ESP_OK) {
        err = bsp_codec_set_rate(sample_rate);
    }
    bsp_codec_mute(false);
    ESP_RETURN_ON_ERROR(err, TAG, "rate switch to %lu failed", (unsigned long)sample_rate);
    s_rate = sample_rate;
    ESP_LOGI(TAG, "sample rate %lu Hz", (unsigned long)sample_rate);
    return ESP_OK;
}

bool bsp_audio_acquire(uint32_t timeout_ms)
{
    return s_owner && xSemaphoreTake(s_owner, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void bsp_audio_release(void)
{
    if (s_owner) {
        xSemaphoreGive(s_owner);
    }
}
