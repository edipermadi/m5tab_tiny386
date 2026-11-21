#!/usr/bin/env python3
"""The Tab5 over its USB-C serial console: microSD files and settings.

    tab5ctl.py ls [dir]
    tab5ctl.py pull <remote> [local]
    tab5ctl.py push <local> [remote]
    tab5ctl.py mv <old> <new>
    tab5ctl.py wifi <ssid> [password]
    tab5ctl.py wifi-forget
    tab5ctl.py tz [Area/City]
    tab5ctl.py status
    tab5ctl.py reboot

Remote paths are relative to the card's root. Needs pyserial, and firmware
built with "SD card file access over the USB-C console" (main/sdfs_serial.c
and main/net_tab5.c describe the protocol). The emulator keeps running
meanwhile; disk images it has open cannot be replaced or renamed.

Opening the port does not reset the board (DTR/RTS are handled the way
idf.py monitor does it). Close any monitor on the port first: only one
program can read it.
"""
import argparse
import base64
import os
import secrets
import sys
import time
import zlib

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("tab5ctl.py needs pyserial: pip install pyserial")

PUT_CHUNK = 8192        # must not exceed SDFS_PUT_CHUNK in sdfs_serial.c
TIMEOUT = 20.0          # seconds without a reply


class DeviceError(Exception):
    pass


def quote(path):
    if '"' in path:
        raise DeviceError(f"{path!r}: names cannot contain '\"'")
    return f'"{path}"'


def find_port():
    for p in list_ports.comports():
        if p.vid == 0x303A:           # Espressif USB Serial/JTAG
            return p.device
    sys.exit("no Espressif USB serial port found; use -p")


class Link:
    def __init__(self, port):
        s = serial.Serial()
        s.port = port
        s.baudrate = 115200           # ignored by USB Serial/JTAG
        s.timeout = 0.5
        # as esp-idf-monitor: lines asserted while opening, then RTS released
        # before DTR, so the chip never sees the reset combination
        s.rts = True
        s.dtr = True
        s.open()
        try:
            s.rts = False
            s.dtr = False
        except OSError:               # no modem lines (a pty, socat, ...)
            pass
        self.s = s
        self.buf = b""

    def close(self):
        self.s.close()

    def send(self, line):
        self.s.write(b"\n@fs " + line.encode() + b"\n")

    def reply(self, timeout=TIMEOUT):
        """next '@fs ...' line, without the prefix (log lines skipped)"""
        deadline = time.monotonic() + timeout
        while True:
            nl = self.buf.find(b"\n")
            if nl >= 0:
                line, self.buf = self.buf[:nl], self.buf[nl + 1:]
                line = line.rstrip(b"\r")
                if line.startswith(b"@fs "):
                    return line[4:].decode(errors="replace")
                continue
            if time.monotonic() > deadline:
                raise DeviceError("no reply from the Tab5 (firmware without sdfs, or busy port?)")
            self.buf += self.s.read(max(1, self.s.in_waiting))

    def expect(self, timeout=TIMEOUT):
        r = self.reply(timeout)
        if r.startswith("err"):
            raise DeviceError(r[4:])
        return r

    def sync(self):
        token = secrets.token_hex(4)
        self.s.reset_input_buffer()
        self.send(f"ping {token}")
        while True:                   # skip leftovers from an earlier run
            if self.reply(5.0) == f"pong {token}":
                return


def progress(done, total, start):
    rate = done / max(time.monotonic() - start, 1e-3)
    pct = 100 * done // total if total else 100
    sys.stderr.write(f"\r{done}/{total} bytes ({pct}%), {rate / 1024:.0f} KB/s ")
    sys.stderr.flush()


def cmd_ls(link, args):
    link.send("ls " + quote(args.path or ""))
    while True:
        r = link.expect()
        if r == "ok":
            return
        kind, _, rest = r.partition(" ")
        if kind == "D":
            print(f"{'<dir>':>12}  {rest}/")
        elif kind == "F":
            size, _, name = rest.partition(" ")
            print(f"{size:>12}  {name}")


def cmd_pull(link, args):
    local = args.local or os.path.basename(args.remote.rstrip("/"))
    if os.path.isdir(local):
        local = os.path.join(local, os.path.basename(args.remote.rstrip("/")))
    link.send("get " + quote(args.remote))
    r = link.expect()
    if not r.startswith("size "):
        raise DeviceError(f"unexpected reply: {r}")
    total = int(r[5:])
    tmp = local + ".part"
    crc, done, start = 0, 0, time.monotonic()
    try:
        with open(tmp, "wb") as f:
            while True:
                r = link.expect()
                if r.startswith("d "):
                    data = base64.b64decode(r[2:])
                    f.write(data)
                    crc = zlib.crc32(data, crc)
                    done += len(data)
                    progress(done, total, start)
                elif r.startswith("ok"):
                    break
                else:
                    raise DeviceError(f"unexpected reply: {r[:60]}")
        sys.stderr.write("\n")
        if done != total or int(r[3:], 16) != crc:
            raise DeviceError("transfer damaged (size or CRC mismatch)")
    except BaseException:
        if os.path.exists(tmp):
            os.remove(tmp)
        raise
    os.replace(tmp, local)
    print(f"{args.remote} -> {local}")


def cmd_push(link, args):
    remote = args.remote or os.path.basename(args.local)
    if remote.endswith("/"):
        remote += os.path.basename(args.local)
    total = os.path.getsize(args.local)
    link.send(f"put {quote(remote)} {total}")
    if link.expect() != "ready":
        raise DeviceError("unexpected reply")
    crc, done, start = 0, 0, time.monotonic()
    try:
        with open(args.local, "rb") as f:
            while True:
                data = f.read(PUT_CHUNK)
                if not data:
                    break
                link.send("d " + base64.b64encode(data).decode())
                crc = zlib.crc32(data, crc)
                done += len(data)
                if link.expect() != f"ack {done}":
                    raise DeviceError("upload out of step")
                progress(done, total, start)
    except BaseException:
        link.send("abort")
        raise
    sys.stderr.write("\n")
    link.send(f"end {crc:08x}")
    link.expect()
    print(f"{args.local} -> {remote}")


def cmd_mv(link, args):
    link.send(f"mv {quote(args.old)} {quote(args.new)}")
    link.expect()


def simple(link, line):
    """a settings command: print its info lines and the text after ok"""
    link.send(line)
    while True:
        r = link.expect()
        if r.startswith("i "):
            print(r[2:])
        elif r == "ok" or r.startswith("ok "):
            if r[3:]:
                print(r[3:])
            return


def cmd_wifi(link, args):
    pw = args.password
    if pw is None:
        import getpass
        pw = getpass.getpass(f"password for {args.ssid} (empty: open network): ")
    simple(link, f"wifi {quote(args.ssid)} {quote(pw)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-p", "--port", help="serial port (default: the first Espressif one)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("ls", help="list a directory")
    p.add_argument("path", nargs="?")
    p = sub.add_parser("pull", help="copy a file from the card")
    p.add_argument("remote")
    p.add_argument("local", nargs="?")
    p = sub.add_parser("push", help="copy a file to the card (replaces it)")
    p.add_argument("local")
    p.add_argument("remote", nargs="?")
    p = sub.add_parser("mv", help="rename or move a file on the card")
    p.add_argument("old")
    p.add_argument("new")
    p = sub.add_parser("wifi", help="save the Wi-Fi network (asks for the password if not given)")
    p.add_argument("ssid")
    p.add_argument("password", nargs="?")
    sub.add_parser("wifi-forget", help="remove the saved Wi-Fi network")
    p = sub.add_parser("tz", help="set the timezone, e.g. Asia/Dubai (or show it)")
    p.add_argument("zone", nargs="?")
    sub.add_parser("status", help="Wi-Fi, timezone and time")
    sub.add_parser("reboot", help="restart the Tab5")
    args = ap.parse_args()

    link = Link(args.port or find_port())
    try:
        link.sync()
        cmds = {
            "ls": cmd_ls, "pull": cmd_pull, "push": cmd_push, "mv": cmd_mv,
            "wifi": cmd_wifi,
            "wifi-forget": lambda l, a: simple(l, "wifi-forget"),
            "tz": lambda l, a: simple(l, "tz" + (" " + quote(a.zone) if a.zone else "")),
            "status": lambda l, a: simple(l, "net"),
            "reboot": lambda l, a: simple(l, "reboot"),
        }
        cmds[args.cmd](link, args)
    except DeviceError as e:
        sys.exit(f"error: {e}")
    except KeyboardInterrupt:
        sys.exit("interrupted")
    finally:
        link.close()


if __name__ == "__main__":
    main()
