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

## Updating from upstream

```sh
git clone https://github.com/hchunhui/tiny386 /tmp/tiny386-upstream
cd /tmp/tiny386-upstream
git diff a6a78937a09203da7cd915ce998893169cce922d HEAD > /tmp/upstream.patch
cd -  # back to this repository
git apply --directory=tiny386 /tmp/upstream.patch
```

Then update the commit above and keep the list of local changes current.
