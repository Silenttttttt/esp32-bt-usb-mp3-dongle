#!/usr/bin/env python3
"""Pure pass-through transport for car_sim.py to talk to the REAL S3's
REAL mounted USB-MSC block device -- built 2026-09-18.

Zero disk-structure simulation. Every READ10 request from car_sim.py is
answered with a raw os.pread() directly against the real block device
(/dev/sda here), byte-for-byte, live -- the exact same kind of read a
real car radio's own USB-MSC/SCSI driver would issue. car_sim.py's own
FAT12 parsing (boot sector, FAT walk, cluster chain) is reading the REAL
S3's REAL, currently-live disk content, not a mirror. This means every
read also goes through the real firmware's own disk_read_at() straddle
protection (fat_disk_shared.h) -- a read too close to the live write
position comes back zero-filled by design, same as it would for a real
car radio's own reader.

Raw read access without root: UDisks2 grants authorized, password-free
read access to a mounted removable block device via its own D-Bus
OpenDevice() method (the same mechanism GNOME Disks/similar tools use for
"create disk image" style raw reads) -- no sudo needed.

Usage:
    python3 real_s3_passthrough.py [device] [port]
    # defaults: /dev/sda, 9003 (car_sim.py's own default --port)

Then run car_sim.py completely unmodified in another terminal:
    python3 car_sim.py --port 9003
"""
import fcntl
import mmap
import os
import socket
import sys

import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

from sector_protocol import recv_read10_request, OPCODE_READ10, SECTOR_SIZE

DEVICE = sys.argv[1] if len(sys.argv) > 1 else "/dev/sda"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 9003

O_DIRECT = 0o40000
DIRECT_ALIGN = 4096


def open_device_fd(device_path):
    """UDisks2 Block.OpenDevice() over D-Bus, returning a real, authorized
    read-only file descriptor to the raw block device -- no root needed.

    REAL BUG FOUND AND FIXED (2026-09-20, live real-hardware testing): this
    used to return the fd as-is, with NO O_DIRECT. Confirmed the SAME page-
    cache-staleness bug class already found and fixed once in car_sim.py's
    own DeviceTransport (see its docstring) -- except here it's WORSE than
    simple staleness, because it silently CORRUPTS FATDISK_ALWAYS_SERVE_
    LIVE's continuity guarantee: a buffered fd lets the Linux kernel issue
    its OWN independent readahead I/O against the real device, entirely
    invisible to and uncoordinated with this script's own os.pread() calls.
    Every readahead-triggered SCSI READ10 ALSO advances the firmware's
    single persistent g_live_read_cursor, exactly like any other read --
    meaning the kernel was silently "stealing" chunks of the live stream
    out from under car_sim.py's own carefully-paced, sequential reads,
    corrupting the stream in a way that looked exactly like a firmware bug
    (confirmed live: byte-for-byte IDENTICAL "Illegal Audio-MPEG-Header"
    failure offsets across three independent test runs spanning two
    separate firmware fixes and two separate board reboots -- a level of
    determinism only explainable by something structural in the PC-side
    read path, not by anything data/timing-dependent in the firmware
    itself). Fixed with O_DIRECT, exactly like DeviceTransport's own fix:
    bypasses the page cache and kernel readahead entirely, so every
    os.pread() genuinely reflects ONLY what was explicitly requested, no
    more and no less."""
    object_path = "/org/freedesktop/UDisks2/block_devices/" + os.path.basename(device_path)
    conn = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
    result, fd_list = conn.call_with_unix_fd_list_sync(
        "org.freedesktop.UDisks2",
        object_path,
        "org.freedesktop.UDisks2.Block",
        "OpenDevice",
        GLib.Variant("(sa{sv})", ("r", {})),
        GLib.VariantType("(h)"),
        Gio.DBusCallFlags.NONE,
        -1,
        None,
        None,
    )
    handle_index = result.unpack()[0]
    fd = fd_list.get(handle_index)
    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags | O_DIRECT)
    return fd


def direct_pread(fd, length, offset):
    """O_DIRECT requires the read buffer's memory address to be aligned, not
    just the file offset/length (already SECTOR_SIZE-multiples here) -- a
    plain os.pread() allocates its own buffer with no alignment guarantee
    and fails EINVAL under O_DIRECT (same fix pattern as car_sim.py's own
    DeviceTransport). An mmap-backed buffer is always page-aligned."""
    buf = mmap.mmap(-1, length)
    try:
        os.preadv(fd, [buf], offset)
        return bytes(buf[:length])
    finally:
        buf.close()


def serve(conn, fd):
    zero_fill_reads = 0
    total_reads = 0
    while True:
        try:
            opcode, lba, count = recv_read10_request(conn)
        except (ConnectionError, OSError):
            print(f"[passthrough] car_sim.py disconnected "
                  f"({zero_fill_reads}/{total_reads} reads were all-zero)", file=sys.stderr)
            return
        if opcode != OPCODE_READ10:
            continue
        data = direct_pread(fd, count * SECTOR_SIZE, lba * SECTOR_SIZE)
        if len(data) < count * SECTOR_SIZE:
            data = data + b"\x00" * (count * SECTOR_SIZE - len(data))
        total_reads += 1
        if data == b"\x00" * len(data):
            zero_fill_reads += 1
        conn.sendall(data)


def main():
    print(f"[passthrough] opening {DEVICE} via UDisks2 OpenDevice (no root)...", file=sys.stderr)
    fd = open_device_fd(DEVICE)
    print(f"[passthrough] got real read-only fd -- every read from here on is a live "
          f"read of the real device, no simulation", file=sys.stderr)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", PORT))
    srv.listen(1)
    print(f"[passthrough] waiting for car_sim.py on port {PORT}...", file=sys.stderr)
    while True:
        conn, _ = srv.accept()
        print("[passthrough] car_sim.py connected", file=sys.stderr)
        serve(conn, fd)


if __name__ == "__main__":
    main()
