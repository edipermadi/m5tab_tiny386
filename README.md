# tiny386 on the M5Stack Tab5

A port of [tiny386](https://github.com/hchunhui/tiny386) (an i386 PC emulator that boots DOS and Windows 9x on microcontrollers) to the **M5Stack Tab5** (ESP32-P4).

- **Display:** the 5" 720x1280 MIPI-DSI panel, used in landscape as a 1280x720 PC screen. The ILI9881C, ST7123 and ST7121 panel variants are detected at boot.
- **Keyboard:** the **M5Stack Tab5 Keyboard** (I2C), and any USB keyboard on the USB-A port.
- **Mouse:** a USB mouse on the USB-A port (HID boot protocol).
- **Sound:** PC speaker, Sound Blaster 16 and AdLib, played through the ES8388 codec and the built-in speaker.
- **Storage:** the microSD card holds the configuration, BIOS images and disk images.

The board support (power and I/O expanders, panel, backlight, audio) comes from the esp32p4_86box BSP. tiny386 is vendored in `tiny386/` (see `tiny386/VENDORED.md` for the upstream commit and the local changes).

## Building

You need ESP-IDF 6.x (tested with 6.1). No ESP-IDF patches are required.

```sh
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

The Tab5's USB-C port is the P4's USB Serial/JTAG. It handles both flashing and the console.

**Chip revision:** `sdkconfig.defaults` builds for pre-v3 ESP32-P4 silicon (`CONFIG_ESP32P4_SELECTS_REV_LESS_V3`). If your Tab5 has a v3.x chip (`esptool chip-id` shows the revision), remove that line, delete `sdkconfig`, and rebuild.

Options in `idf.py menuconfig` → *tiny386 on M5Stack Tab5*:

| Option | Default |
|---|---|
| Screen orientation (landscape 90 or 270). Switch it if the picture is upside down with the keyboard attached. | 270 |
| Backlight brightness | 60 % |
| PSRAM pool for the emulator | 24 MB |
| Tab5 Keyboard I2C port, SDA, SCL, address | port 0, G0, G1, 0x6D |
| USB host on the USB-A port | on |

## SD card

Format the card as FAT32. Disk images must stay under 4 GB, because ESP-IDF's FatFs is built without exFAT. Put these files in the root of the card:

```
tiny386.ini      start from sdcard/tiny386.ini
bios.bin         SeaBIOS, from the tiny386 releases page
vgabios.bin
win95.img        your disk image (hda = ...)
```

Without a card, the firmware mounts the `storage` FAT partition in flash at `/spiflash` and looks for `/spiflash/tiny386.ini` there.

## Tab5 Keyboard mapping

| Key | Sends |
|---|---|
| Aa | Shift |
| Ctrl, Alt | Ctrl, Alt |
| `! @ # $ % ^ & * ( ) + _` | the shifted PC key (Shift is added automatically) |
| Sym + `1`…`0`, `-`, `+` | F1…F10, F11, F12 |
| Sym + Up / Down | Page Up / Page Down |
| Sym + Left / Right | Home / End |
| Sym + Del | Insert |
| Sym + Backspace | Delete |
| Sym + Esc | `` ` `` |
| Sym + Tab | Caps Lock |
| Sym + Alt | Windows key |
| Sym + `` ` ! * ( ) [ ] \ ; ' _ . `` | `~ ? / < > { } \| : " = ,` (the second legends) |

## Not done yet

- Wi-Fi (via the ESP32-C6) and the emulated NE2000 network card.
- Touch screen input.

## Licenses

- tiny386: BSD-3-Clause (vendored in `tiny386/`)
- `main/bsp/`: from esp32p4_86box (MIT). The Tab5 panel init tables come from M5Stack (Apache-2.0).
