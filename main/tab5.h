/* Glue between the Tab5 port's modules. */
#pragma once

#include <stdint.h>
#include "sdkconfig.h"

/* lcd_tab5.c */
void vga_task(void *arg);
int lcd_strip_count(void);
/* copies one strip of the emulator frame buffer to the panel (with rotation) */
void lcd_present_strip(const uint16_t *src_fb, int strip);

/* audio_tab5.c */
void audio_start(void);

/* storage_tab5.c: mounts the SD card at /sdcard (or the flash FAT at /spiflash) */
void storage_init(void);

/* kbd_tab5.c */
void tab5_kbd_start(void);

/* tiny386 esp/main/usb_input.c */
void usb_setup(void);
