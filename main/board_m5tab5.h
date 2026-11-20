/*
 * tiny386 build options for the M5Stack Tab5 (ESP32-P4, 720x1280 MIPI-DSI panel
 * used in landscape as 1280x720). Force-included into every source of the
 * main component (see main/CMakeLists.txt), as tiny386's own esp/ port does
 * with its board_*.h files.
 */
#pragma once

#include "sdkconfig.h"

#define BUILD_ESP32

/* Emulator heap carved out of PSRAM (the Tab5 has 32 MB): guest RAM, VGA RAM
 * and the 1280x720 console frame buffer come from here. */
#define PSRAM_ALLOC_LEN (CONFIG_TAB5_PSRAM_POOL_MB * 1024 * 1024)

/* see tiny386 esp/main/board_jc4880p443.h */
#define IRAM_ATTR_CPU_EXEC1

/* Frame buffer: RGB565, in the panel's native (portrait) layout. With SWAPXY
 * the VGA code writes the landscape picture transposed, so LCD_WIDTH (1280) runs
 * down the panel and LCD_HEIGHT (720) across it. */
#define BPP 16
#define FULL_UPDATE
#define SWAPXY
#define LCD_WIDTH 1280
#define LCD_HEIGHT 720

/* microSD: SDMMC slot 0, IO_MUX pins, IO rail from on-chip LDO VO4 */
#define SD_CLK 43
#define SD_CMD 44
#define SD_D0 39
#define SD_D1 40
#define SD_D2 41
#define SD_D3 42
#define SD_PWR_CTRL_LDO_IO_ID 4

#define MIXER_BUF_LEN 512

/* ide.c: gather the blocks of a multi-sector write into one SD write (64 KB
 * buffer); the command still completes only once the data is on the card */
#define IDE_ENABLE_WRITE_GATHER

/* ide.c: disk and CD images on the SD card through main/hdd_cache.c (raw
 * sector DMA and a PSRAM read cache) instead of stdio */
#define IDE_ENABLE_HDD_CACHE
