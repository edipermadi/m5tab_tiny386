/* Glue between the Tab5 port's modules. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"

/* lcd_tab5.c */
void vga_task(void *arg);
int lcd_strip_count(void);
/* copies one strip of the emulator frame buffer to the panel (with rotation) */
void lcd_present_strip(const uint16_t *src_fb, int strip);
/* width of the black bar on each side of the picture (landscape pixels), 0 if
   none (or too narrow for the disk LEDs) */
int lcd_bar_width(void);

/* touch_tab5.c: touch screen as a touchpad on the PS/2 mouse */
void touch_start(void);

/* audio_tab5.c */
void audio_start(void);

/* storage_tab5.c: mounts the SD card at /sdcard (or the flash FAT at /spiflash) */
void storage_init(void);

/* sdfs_serial.c: SD card file access over the USB-C console (tools/tab5ctl.py) */
void sdfs_serial_start(void);
/* one "@fs ..." reply line to the console; false if the host stopped reading */
bool sdfs_reply(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* files the emulator has open: not to be replaced or renamed */
void sdfs_set_busy(const char *const *paths, int n);

/* net_tab5.c: the clock (Wi-Fi via the ESP32-C6 + SNTP, once at boot) and the
   timezone, set from the console. net_init() blocks until the time is known
   (or the timeout), then switches Wi-Fi and the C6 off. Call it before
   storage_init() (the C6 link uses the SD card's SDMMC controller) and
   before USB and the display (it needs internal RAM while it runs). */
void net_init(void);
/* a console command for it: false if not one */
bool net_command(int argc, char **argv);

/* kbd_tab5.c */
void tab5_kbd_start(void);

/* tiny386 esp/main/usb_input.c */
void usb_setup(void);
