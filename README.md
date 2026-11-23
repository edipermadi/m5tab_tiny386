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
| CPU emulation speed-up: tiny386's fast path in front of the interpreter, optionally with pre-decoding of hot code (about 3 MB more PSRAM) | fast path with pre-decoding |
| PSRAM pool for the emulator | 24 MB |
| Tab5 Keyboard I2C port, SDA, SCL, address | port 0, G0, G1, 0x6D |
| USB host on the USB-A port | on |

## SD card

Format the card as FAT32. Disk images must stay under 4 GB, because ESP-IDF's FatFs is built without exFAT. Put these files in the root of the card:

```
tiny386.ini      start from sdcard/tiny386.ini
bios.bin         SeaBIOS, built from tiny386/seabios (below)
vgabios.bin
win95.img        your disk image (hda = ...)
```

The BIOS images come from SeaBIOS with this repository's patch, `tiny386/seabios/patch`. It is upstream tiny386's patch plus APM battery status, which shows the Tab5's battery to the guest. The `bios.bin` from the tiny386 releases page still boots, but the guest then sees AC power and no battery. To build them (host gcc with `-m32` support and python3):

```sh
git clone https://github.com/coreboot/seabios.git && cd seabios
patch -p1 < ../m5tab_tiny386/tiny386/seabios/patch
cp ../m5tab_tiny386/tiny386/seabios/config .config
make olddefconfig && make
# out/bios.bin, out/vgabios.bin
```

### Battery in the guest

The guest's APM BIOS reports the Tab5's battery: the charge level (from the cell voltage, 3.3-4.15 V), charging, and AC (USB-C) power. Windows 9x shows it in the Power Meter once APM support is enabled in Device Manager. Above 25 % the battery reads high, from 10 % low, below that critical. It updates every 5 seconds. The Tab5 has no USB-C power sense, so AC is inferred from the battery current: AC means the battery is not discharging.

Without a card, the firmware mounts the `storage` FAT partition in flash at `/spiflash` and looks for `/spiflash/tiny386.ini` there.

### Files and settings over USB-C

`tools/tab5ctl.py` reaches the SD card over the USB-C port while the emulator runs, so the card can stay in the Tab5. It needs pyserial.

```sh
tools/tab5ctl.py ls [dir]
tools/tab5ctl.py pull <remote> [local]
tools/tab5ctl.py push <local> [remote]     # replaces an existing file
tools/tab5ctl.py mv <old> <new>
tools/tab5ctl.py wifi <ssid> [password]    # asks for the password if left out
tools/tab5ctl.py wifi-forget
tools/tab5ctl.py tz Asia/Dubai              # IANA name; without one, shows it
tools/tab5ctl.py status                     # Wi-Fi, timezone, time
tools/tab5ctl.py reboot
```

Remote paths are relative to the card's root. The port is found automatically (`-p /dev/ttyACM0` to pick one). Opening it does not reset the board. Close `idf.py monitor` first, since only one program can read the port.
- **Uploads are safe to interrupt:** a push goes to `<name>.part` and replaces the file only after its size and CRC check out.
- **Open images are protected:** the disk and floppy images named in `tiny386.ini` cannot be replaced or renamed while the emulator has them open. Pulling one gives a copy that can be inconsistent if the guest writes meanwhile.

### Wi-Fi and the clock

The Tab5 has no battery-backed clock, so without the network the guest starts in 1970. Save a Wi-Fi network with `tab5ctl.py wifi`, then reboot. From then on, every boot connects through the ESP32-C6, gets the time over NTP, then switches Wi-Fi and the C6 off and starts the PC. If the time does not arrive within 30 s, the PC starts anyway; `tab5ctl.py status` says why. The guest's clock shows local time in the zone set with `tab5ctl.py tz` (UTC if none). A timezone change applies at once, without a reboot. The settings are kept in NVS, so they survive reflashing the firmware, but `idf.py erase-flash` clears them. The NTP server and the wait are in menuconfig.

The C6's factory firmware (esp-hosted 1.x) works with the ESP-Hosted 3.1 host used here. Wi-Fi is used for nothing else; the emulated network card is not connected to it.

It can be switched off in menuconfig (*SD card file access over the USB-C console*). The protocol is described in `main/sdfs_serial.c`.

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

- The emulated NE2000 network card (Wi-Fi itself is used only for the clock).
- Touch screen input.

## Licenses

- tiny386: BSD-3-Clause (vendored in `tiny386/`)
- `main/bsp/`: from esp32p4_86box (MIT). The Tab5 panel init tables come from M5Stack (Apache-2.0).
