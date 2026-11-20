/*
 * microSD on SDMMC slot 0 (4-bit, IO_MUX pins), IO rail from on-chip LDO VO4,
 * FAT mounted at /sdcard. Without a card, the "storage" flash partition is
 * mounted at /spiflash instead. Based on tiny386 esp/main/storage.c.
 */
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

#include "tab5.h"

static const char *TAG = "storage";

void *rawsd;     /* used by tiny386's ide.c for raw SD block devices */
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

static bool mount_sd(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = SD_PWR_CTRL_LDO_IO_ID,
    };
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
    esp_err_t ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD LDO power control failed: %s", esp_err_to_name(ret));
        return false;
    }
    host.pwr_ctrl_handle = pwr_ctrl_handle;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk = SD_CLK;
    slot_config.cmd = SD_CMD;
    slot_config.d0 = SD_D0;
    slot_config.d1 = SD_D1;
    slot_config.d2 = SD_D2;
    slot_config.d3 = SD_D3;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card mount failed: %s", esp_err_to_name(ret));
        return false;
    }
    sdmmc_card_print_info(stdout, card);
    rawsd = card;
    return true;
}

void storage_init(void)
{
    if (mount_sd()) {
        return;
    }
    const esp_vfs_fat_mount_config_t cfg = {
        .max_files = 4,
        .format_if_mount_failed = false,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    esp_err_t ret = esp_vfs_fat_spiflash_mount_rw_wl("/spiflash", "storage", &cfg, &s_wl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "flash FAT mount failed: %s", esp_err_to_name(ret));
    }
}
