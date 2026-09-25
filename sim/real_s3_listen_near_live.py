#!/usr/bin/env python3
"""Variant of real_s3_listen.py that starts reading NEAR THE CURRENT LIVE
EDGE of the ring instead of always at byte 0 -- built 2026-09-20 after
extensive live testing showed real_s3_listen.py (and every other reader
tonight, all of which start cold at byte 0) reliably serves genuinely
STALE content: reading sequentially from byte 0 at 1x real-time pace means
hearing whatever was written there THE LAST TIME the writer passed through
that exact offset, which for a continuously-running, already-wrapped ring
is structurally up to one full ring-duration old (currently ~240s / 4min)
-- confirmed directly live tonight ("i hear extremely stale stuff").

This is NOT a fix to the firmware or the ring design -- it's a bench-test-
only technique unavailable to the real, dumb SCSI-only car radio (which has
no side channel to ask "where are you writing right now"). It exists to
let bench testing hear ACTUAL current content while the real fix (either
some form of client-side bounded catch-up read-ahead that could plausibly
explain how the real Kenwood stays near-live, or a firmware-side dynamic
resize -- see progress/PLAN_NEXT.md's C1/C4 sections) is still being
figured out.

Gets the live write position via the classic ESP32's own 'C' control
channel (S3_RX:S3_HB:N,write_pos=N heartbeat, forwarded by the classic's
poll_return_serial() from the S3's own return channel) -- a real,
already-working diagnostic path from tonight's session, not something new.

Usage:
    python3 real_s3_listen_near_live.py [device] [classic_serial_port]
    # defaults: /dev/sda, /dev/ttyACM1
"""
import fcntl
import mmap
import os
import re
import subprocess
import sys
import termios
import time
import tty

import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

DEVICE = sys.argv[1] if len(sys.argv) > 1 else "/dev/sda"
CLASSIC_PORT = sys.argv[2] if len(sys.argv) > 2 else "/dev/ttyACM1"

SECTOR_SIZE = 512
FIRST_DATA_LBA = 4
DECLARED_FILE_SIZE = 3842048
BITRATE_BYTES_PER_SEC = 16000
READ_CHUNK = 4096
# Safely behind the live edge -- clear of the firmware's own
# READ_MARGIN_BYTES (2 clusters = 8192 bytes) straddle-protection zone,
# with real margin to spare. Matches the same safe-capture technique used
# repeatedly (and confirmed working) elsewhere tonight.
SAFETY_MARGIN_BYTES = 100000

O_DIRECT = 0o40000
DIRECT_ALIGN = 4096


def open_device_fd(device_path):
    object_path = "/org/freedesktop/UDisks2/block_devices/" + os.path.basename(device_path)
    conn = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
    result, fd_list = conn.call_with_unix_fd_list_sync(
        "org.freedesktop.UDisks2", object_path,
        "org.freedesktop.UDisks2.Block", "OpenDevice",
        GLib.Variant("(sa{sv})", ("r", {})),
        GLib.VariantType("(h)"), Gio.DBusCallFlags.NONE, -1, None, None)
    fd = fd_list.get(result.unpack()[0])
    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags | O_DIRECT)
    return fd


def direct_pread(fd, length, offset):
    align_start = (offset // DIRECT_ALIGN) * DIRECT_ALIGN
    align_end = ((offset + length + DIRECT_ALIGN - 1) // DIRECT_ALIGN) * DIRECT_ALIGN
    buf = mmap.mmap(-1, align_end - align_start)
    os.lseek(fd, align_start, os.SEEK_SET)
    os.readv(fd, [buf])
    result = bytes(buf[offset - align_start: offset - align_start + length])
    buf.close()
    return result


def get_live_write_pos(port, timeout_s=5.0):
    """Opens the classic's serial port raw, reads until an S3_RX:S3_HB
    heartbeat line reveals the current write_pos, then closes -- a single,
    one-shot query, not a persistent tap (avoids competing with anything
    else that might read this port)."""
    fd = os.open(port, os.O_RDONLY | os.O_NOCTTY)
    try:
        tty.setraw(fd)
        attrs = termios.tcgetattr(fd)
        attrs[4] = attrs[5] = termios.B921600
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        deadline = time.monotonic() + timeout_s
        buf = b""
        while time.monotonic() < deadline:
            chunk = os.read(fd, 4096)
            buf += chunk
            m = re.search(rb"write_pos=(\d+)", buf)
            if m:
                return int(m.group(1))
            if len(buf) > 65536:
                buf = buf[-4096:]  # keep it bounded, don't grow forever
        raise TimeoutError(f"no write_pos heartbeat seen on {port} within {timeout_s}s")
    finally:
        os.close(fd)


def main():
    print(f"[listen-near-live] querying live write_pos from {CLASSIC_PORT}...", file=sys.stderr)
    write_pos = get_live_write_pos(CLASSIC_PORT)
    print(f"[listen-near-live] live write_pos={write_pos}", file=sys.stderr)

    print(f"[listen-near-live] opening {DEVICE} via UDisks2 OpenDevice (no root)...", file=sys.stderr)
    fd = open_device_fd(DEVICE)
    print("[listen-near-live] got real fd, starting mpg123", file=sys.stderr)

    player = subprocess.Popen(["mpg123", "--resync-limit", "-1", "-"], stdin=subprocess.PIPE)
    data_start = FIRST_DATA_LBA * SECTOR_SIZE

    # Start SAFETY_MARGIN_BYTES behind the live edge, cluster-aligned.
    start_pos = max(0, write_pos - SAFETY_MARGIN_BYTES)
    start_pos -= start_pos % READ_CHUNK
    print(f"[listen-near-live] starting playback at pos={start_pos} "
          f"({SAFETY_MARGIN_BYTES} bytes behind the live edge)", file=sys.stderr)

    pos = start_pos
    next_send_time = time.monotonic()
    try:
        while True:
            chunk = direct_pread(fd, READ_CHUNK, data_start + pos)
            try:
                player.stdin.write(chunk)
                player.stdin.flush()
            except BrokenPipeError:
                print("[listen-near-live] player exited", file=sys.stderr)
                break
            pos = (pos + READ_CHUNK) % DECLARED_FILE_SIZE
            next_send_time += READ_CHUNK / BITRATE_BYTES_PER_SEC
            sleep_for = next_send_time - time.monotonic()
            if sleep_for > 0:
                time.sleep(sleep_for)
            else:
                next_send_time = time.monotonic()
    except KeyboardInterrupt:
        print("\n[listen-near-live] stopped", file=sys.stderr)
    finally:
        try:
            player.stdin.close()
        except Exception:
            pass
        player.terminate()


if __name__ == "__main__":
    main()
