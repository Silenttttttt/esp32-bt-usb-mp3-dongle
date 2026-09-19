#!/usr/bin/env python3
"""Standalone, robust listener for the REAL S3's real mounted USB-MSC
device -- built 2026-09-18 after ffmpeg's ultra-low-latency config
(car_sim.py's own player) proved unable to cleanly resync after the rare
(measured: ~3 breaks per 625 real frame transitions, 99.5% clean) content
discontinuities inherent to reading a continuously-overwritten ring
buffer. Does NOT touch car_sim.py or any of its own code at all.

Uses mpg123 instead of ffmpeg -- a decoder built for real-world streaming
(internet radio, which has the exact same "occasional real discontinuity,
must resync and keep playing" requirement) rather than one configured for
minimum possible latency. Reads the real device directly via the same
UDisks2 OpenDevice() no-root mechanism as real_s3_passthrough.py, tracks
its own read position sequentially through the ring (wrapping at the end,
matching "repeat track" -- the correct, intended behavior for a static/
paused source), and paces itself at the real encode bitrate instead of
racing ahead and re-reading the same relative ring position every cycle
(the bug in the very first throwaway version of this idea tonight).
"""
import ctypes
import fcntl
import mmap
import os
import subprocess
import sys
import time

import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

DEVICE = sys.argv[1] if len(sys.argv) > 1 else "/dev/sda"

SECTOR_SIZE = 512
FIRST_DATA_LBA = 4          # matches fat_disk_shared.h's current layout
DECLARED_FILE_SIZE = 3842048  # matches fat_disk_shared.h's current 938-cluster (~4min) ring
BITRATE_BYTES_PER_SEC = 16000  # 128kbps
READ_CHUNK = 4096  # one cluster

# REAL BUG FOUND (2026-09-18, real hardware, real end-to-end test): a
# standard buffered os.pread() against the raw block device path was
# silently being served from the kernel's own buffer/page cache on
# repeat reads at the same offset instead of re-issuing a real SCSI
# READ(10) to the device -- posix_fadvise(..., POSIX_FADV_DONTNEED)
# before each read did NOT fix this (confirmed directly, byte-identical
# content with and without it). Proved definitively by comparing against
# an O_DIRECT read at the same position across 4 full ring laps (~28s
# apart): O_DIRECT returned genuinely different content every single
# time, while plain buffered pread() returned 100% identical content
# across 3 full laps in a row. The real firmware/ring/hardware was never
# the bug -- this Python tool was feeding mpg123 stale cached bytes the
# whole time. O_DIRECT (enabled here via fcntl F_SETFL after the
# UDisks2-provided fd is opened -- Linux allows this post-open, unlike
# most other O_DIRECT-requiring platforms) forces every read to be a
# genuine live request to the device, matching what a real car radio's
# own block-level reads would actually observe.
O_DIRECT = 0o40000  # Linux-specific flag value (no portable constant in os/fcntl)
DIRECT_ALIGN = 4096  # safe alignment for O_DIRECT on this device/controller


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
    """O_DIRECT read requiring page-aligned offset/length/buffer. offset
    and length here (READ_CHUNK=4096, FIRST_DATA_LBA*SECTOR_SIZE=2048)
    are not always DIRECT_ALIGN-aligned on their own, so this reads a
    slightly larger DIRECT_ALIGN-aligned window covering the requested
    range and slices out just the needed bytes."""
    align_start = (offset // DIRECT_ALIGN) * DIRECT_ALIGN
    align_end = ((offset + length + DIRECT_ALIGN - 1) // DIRECT_ALIGN) * DIRECT_ALIGN
    buf = mmap.mmap(-1, align_end - align_start)
    os.lseek(fd, align_start, os.SEEK_SET)
    os.readv(fd, [buf])
    result = bytes(buf[offset - align_start: offset - align_start + length])
    buf.close()
    return result


def main():
    print(f"[listen] opening {DEVICE} via UDisks2 OpenDevice (no root)...", file=sys.stderr)
    fd = open_device_fd(DEVICE)
    print("[listen] got real fd, starting mpg123 (robust decoder, not ffmpeg)", file=sys.stderr)

    # --resync-limit: mpg123's DEFAULT resync search window (1024 bytes) is
    # smaller than a single straddle-protected zero-fill gap in the ring
    # (up to one cluster, 4096 bytes -- see fat_disk_shared.h's
    # disk_read_at()) -- confirmed live: mpg123 hit "Giving up resync after
    # 1024 bytes" and exited entirely the first time a real gap was hit,
    # rather than skipping past it like it does for smaller glitches. -1
    # means search the whole stream for the next valid sync, matching how
    # a real, patient MP3 decoder should behave against this ring design.
    player = subprocess.Popen(["mpg123", "--resync-limit", "-1", "-"], stdin=subprocess.PIPE)
    data_start = FIRST_DATA_LBA * SECTOR_SIZE

    pos = 0
    next_send_time = time.monotonic()
    try:
        while True:
            chunk = direct_pread(fd, READ_CHUNK, data_start + pos)
            try:
                player.stdin.write(chunk)
                player.stdin.flush()
            except BrokenPipeError:
                print("[listen] player exited", file=sys.stderr)
                break
            pos = (pos + READ_CHUNK) % DECLARED_FILE_SIZE
            # Pace at the real encode bitrate so we're reading sequentially
            # at roughly the same rate the ring is being written, instead
            # of racing ahead (which causes the whole-ring-aliasing bug
            # from the first throwaway version of this idea).
            next_send_time += READ_CHUNK / BITRATE_BYTES_PER_SEC
            sleep_for = next_send_time - time.monotonic()
            if sleep_for > 0:
                time.sleep(sleep_for)
            else:
                next_send_time = time.monotonic()
    except KeyboardInterrupt:
        print("\n[listen] stopped", file=sys.stderr)
    finally:
        try:
            player.stdin.close()
        except Exception:
            pass
        player.terminate()


if __name__ == "__main__":
    main()
