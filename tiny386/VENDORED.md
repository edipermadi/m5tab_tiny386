# Vendored tiny386

This directory holds a copy of [tiny386](https://github.com/hchunhui/tiny386)
(BSD-3-Clause, see `LICENSE`). It is vendored rather than a submodule so this
port can change it.

- Upstream: https://github.com/hchunhui/tiny386
- Based on commit: `a6a78937a09203da7cd915ce998893169cce922d`
  ("i386: BOUND with a register operand raises #UD")

## Local changes

- `vga.c`, `vga.h`: `vga_get_picture_size()` reports the size of the picture
  last drawn into the frame buffer, so the Tab5 display code can scale it up
  to fill the panel (`main/lcd_tab5.c`).
- `ide.c`: with `IDE_ENABLE_HDD_CACHE`, hard disk and CD images go through a
  block device backed by the port's `hdd_cache.h` (raw SD sectors and a PSRAM
  read cache, `main/hdd_cache.c`) when it can take them, else through stdio
  as before; CD changes switch between the two.
- `ne2000.c`: on a chip without Wi-Fi (`!SOC_WIFI_SUPPORTED`, the ESP32-P4)
  the NIC's MAC is the chip's base MAC, locally administered, instead of the
  Wi-Fi station MAC it does not have.
- `pc.c`, `pc.h`: `PC.host_power`, read at port 0x8902, carries the host's
  battery status (charge level, AC, charging) for the BIOS. The Tab5 code
  fills it from the battery monitor (`main/esp_main.c`).
- `esp/main/usb_input.c`: a USB HID device that cannot be opened or started
  (out of memory, say) is logged and skipped instead of aborting.
- `misc.c`: the CMOS clock is local time (`localtime_r`), as DOS and Windows
  expect, instead of UTC; with no TZ set the two are the same.
- `seabios/patch`: APM "get power status" (INT 15h AX=530Ah) reports
  `PC.host_power` instead of "AC, no battery".

## Updating from upstream

```sh
git clone https://github.com/hchunhui/tiny386 /tmp/tiny386-upstream
cd /tmp/tiny386-upstream
git diff a6a78937a09203da7cd915ce998893169cce922d HEAD > /tmp/upstream.patch
cd -  # back to this repository
git apply --directory=tiny386 /tmp/upstream.patch
```

Then update the commit above and keep the list of local changes current.
