#!/usr/bin/env python3
"""Synthetic stand-in for the car radio. This is a genuine FAT12 client --
it parses the boot sector's BPB, reads the FAT table, finds the file in the
root directory, and walks its cluster chain, exactly like a real embedded
filesystem driver would. It does NOT know the disk layout in advance and
does not import fat12_disk.py -- everything it knows comes from parsing
bytes it reads over the same READ10 boundary a real device would expose.
Only ever talks to that one boundary, so it doesn't know or care whether
the other end is s3_sim_serial.py or a real S3 presenting real USB Mass
Storage.

Deliberately as dumb and direct as possible, matching the real target: an
old (~2010-era) aftermarket head unit that only plays CDs and a USB stick
of MP3s. It has no side channel to the disk, no way to ask "is there
anything new yet," and no smart buffering strategy of its own beyond
whatever small hardware FIFO its decoder chip has -- it just reads the
next cluster in the FAT chain and feeds the bytes to its decoder,
synchronously, one read at a time. On reaching the end of the chain it
starts over from the first cluster, same as "repeat track" on a real
unit -- not a Python-specific trick. There is no reader thread, no
read-ahead buffer, and no bitrate-based pacing anywhere in this file: the
only backpressure is the decoder's own stdin pipe blocking once it's full,
exactly like a real decoder chip's input FIFO would. An earlier version
paced itself against an assumed encoder bitrate to fake real-time
playback for measurement purposes -- that constant was never quite right
(true rate vs. assumed rate) and caused its own slow-building drift bug.
Removing it removes that whole class of bug along with the assumption.
"""
import argparse
import gc
import json
import mmap
import os
import queue
import re
import socket
import subprocess
import sys
import threading
import time

from sector_protocol import SECTOR_SIZE, read_sectors

# Used by HelperDeviceTransport (see its own docstring) to automate the one
# operation that needs root (opening the real S3 block device's fd) without
# an interactive prompt. Per Muni's own explicit request (2026-09-21) --
# this password is already established, trusted, and freely-reusable for
# sudo on this machine (see this session's own saved memory on the
# subject).
def _load_sudo_password():
    # Never hardcode this (the repo has a public-able remote). Env var first,
    # then a local git-ignored file next to this script.
    pw = os.environ.get("CAR_SIM_SUDO_PASSWORD")
    if pw:
        return pw
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".sudo_password")
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return ""


SUDO_PASSWORD = _load_sudo_password()


# ===================== Transport abstraction (2026-09-20) ====================
#
# Everything else in this file (parse_boot_sector, find_all_file_entries,
# walk_cluster_chain) already only ever operates on raw bytes it's handed --
# it never touches a socket or file directly. So the ONLY thing that needs
# to know whether we're talking to a fake TCP stand-in or a REAL physical S3
# board's real USB-MSC drive is this one small abstraction.
class TcpTransport:
    """The original transport: sector_protocol.py's fake READ10-over-TCP
    wire format, talking to a PC-hosted S3 stand-in (s3_sim_serial.py /
    s3_real_firmware_host[_multi])."""
    def __init__(self, sock):
        self.sock = sock

    def read(self, lba, count):
        return read_sectors(self.sock, lba, count)


class DeviceTransport:
    """Reads directly from a REAL block device -- e.g. the real physical S3
    board's own real USB-MSC drive, once its native USB-OTG port is plugged
    into this machine and the OS enumerates it (appears as /dev/sdX, exactly
    like plugging in any real USB flash drive). No fake protocol at all:
    this is genuinely the same access a real car radio's own USB-MSC host
    driver would have to a real drive. Needs read permission on the device
    node -- typically root, or membership in the OS's disk/storage group;
    this deliberately does NOT escalate its own privileges (run this
    yourself with sudo if needed, same as any other real hardware access).

    REAL BUG FOUND (2026-09-20, first live real-S3-hardware test tonight):
    a plain buffered open() lets Linux's page cache serve STALE blocks for
    this device -- confirmed directly, reading the exact same LBA twice, 5
    real seconds apart, returned byte-for-byte identical data despite the
    ring having been continuously rewritten with real audio the entire
    time. This is a real, previously-undiscovered gap specific to reading a
    growing/looping ring THROUGH THE LINUX BLOCK LAYER: the kernel assumes
    a block device's content at a given LBA doesn't change unless caused by
    a write THROUGH THAT SAME fd, which is false for this device by design
    (the S3 rewrites the same physical LBAs with new content as the ring
    wraps, entirely outside the kernel's own knowledge). A real dumb car
    radio has no page cache at all and would never hit this -- it's purely
    a PC-testing-methodology artifact, not a firmware/pipeline bug. Fixed
    with O_DIRECT, which bypasses the page cache and forces every read to
    genuinely hit the device fresh -- exactly matching what happens on
    real, non-caching embedded hardware. Requires the buffer size to be a
    multiple of the device's logical block size (512 bytes here, always
    true since callers request whole SECTOR_SIZE-multiples) -- os.pread's
    result buffer is allocated by the OS to satisfy this automatically."""
    def __init__(self, path):
        self.path = path
        self.fd = os.open(path, os.O_RDONLY | os.O_DIRECT)

    def read(self, lba, count):
        # O_DIRECT requires the READ BUFFER's memory address to be aligned
        # (not just the file offset/length, which are already
        # SECTOR_SIZE-multiples from every caller) -- a plain os.pread()
        # allocates its own buffer with no alignment guarantee, which
        # fails with EINVAL under O_DIRECT (confirmed directly). An
        # mmap-backed buffer is always page-aligned, satisfying every real
        # device's alignment requirement; os.preadv() can read directly
        # into one.
        length = count * SECTOR_SIZE
        buf = mmap.mmap(-1, length)
        try:
            os.preadv(self.fd, [buf], lba * SECTOR_SIZE)
            return buf[:length]
        finally:
            buf.close()


class UdisksDeviceTransport(DeviceTransport):
    """Same real, direct block-device access as DeviceTransport (identical
    read() -- inherited unchanged), but opens the fd via UDisks2's
    Block.OpenDevice() D-Bus method instead of a plain os.open(). This is
    the SAME passwordless-access mechanism real_s3_passthrough.py has used
    all along, but used INLINE here instead of via a separate TCP-bridging
    process/port.

    REAL BUG FOUND (2026-09-21, first live real-hardware test after
    switching repro_live_v2.py from the TCP-passthrough architecture to a
    plain `sudo`+DeviceTransport one): running the WHOLE script (including
    the mpg123 player subprocess) as root broke real audio output --
    confirmed live: "didn't even play the right song, played for a sec"
    matches PipeWire/PulseAudio silently dropping a root-owned client's
    audio stream shortly after an initial handshake, even with `sudo -E`
    preserving PULSE_SERVER/XDG_RUNTIME_DIR (those env vars aren't the only
    thing the audio server checks -- the connecting process's real UID
    matters too). This is EXACTLY the problem UDisks2's OpenDevice() was
    already solving for real_s3_passthrough.py: passwordless, non-root
    access to the raw block device, so the WHOLE process (audio player
    included) can keep running as the normal user throughout, with zero
    privilege elevation needed anywhere. The TCP port itself was never
    required for that -- it was just an incidental side effect of
    real_s3_passthrough.py happening to be a separate bridging process.
    This class gets the same UDisks2 benefit with neither a port nor a
    separate process nor root."""
    def __init__(self, path):
        self.path = path
        import fcntl
        import gi
        gi.require_version("Gio", "2.0")
        from gi.repository import Gio, GLib
        O_DIRECT = 0o40000
        object_path = "/org/freedesktop/UDisks2/block_devices/" + os.path.basename(path)
        conn = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
        result, fd_list = conn.call_with_unix_fd_list_sync(
            "org.freedesktop.UDisks2", object_path,
            "org.freedesktop.UDisks2.Block", "OpenDevice",
            GLib.Variant("(sa{sv})", ("r", {})),
            GLib.VariantType("(h)"), Gio.DBusCallFlags.NONE, -1, None, None)
        handle_index = result.unpack()[0]
        self.fd = fd_list.get(handle_index)
        flags = fcntl.fcntl(self.fd, fcntl.F_GETFL)
        fcntl.fcntl(self.fd, fcntl.F_SETFL, flags | O_DIRECT)


class HelperDeviceTransport(DeviceTransport):
    """Same real, direct block-device access as DeviceTransport (identical
    read() -- inherited unchanged), but gets the fd from a tiny, separate
    ROOT-ONLY helper process (open_device_fd_helper.py) via Unix-socket
    SCM_RIGHTS fd-passing, instead of opening it itself.

    REAL BUG FOUND (2026-09-21, live test): UdisksDeviceTransport's
    passwordless D-Bus OpenDevice() call triggered an interactive polkit
    password PROMPT on this system (not actually passwordless here,
    despite working that way for real_s3_passthrough.py's own long-running
    use tonight) -- not the fully-automated flow needed for repeated quick
    test runs. Muni explicitly asked for this to be automated with his own
    sudo password. Using plain `sudo` for the WHOLE script was already
    ruled out (breaks mpg123's real audio output, see UdisksDeviceTransport's
    own docstring) -- so only the ONE operation that actually needs root
    (opening the device fd) runs under sudo, via a small, separate,
    short-lived helper process; everything else, mpg123 included, stays
    running as the normal user the entire time, exactly like a plain
    `car_sim.py --gui` run. The helper connects out to a Unix socket this
    class creates and listens on, sends the fd, then exits immediately."""
    def __init__(self, path, sudo_password):
        self.path = path
        import socket as _socket
        import subprocess as _subprocess
        import tempfile

        helper_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "open_device_fd_helper.py")
        with tempfile.TemporaryDirectory() as tmpdir:
            socket_path = os.path.join(tmpdir, "fd_helper.sock")
            listener = _socket.socket(_socket.AF_UNIX, _socket.SOCK_STREAM)
            listener.bind(socket_path)
            listener.listen(1)

            proc = _subprocess.Popen(
                ["sudo", "-S", "-k", "python3", helper_path, path, socket_path],
                stdin=_subprocess.PIPE, stdout=_subprocess.DEVNULL,
                stderr=_subprocess.PIPE)
            proc.stdin.write((sudo_password + "\n").encode())
            proc.stdin.close()

            listener.settimeout(10.0)
            conn, _ = listener.accept()
            _, fds, _, _ = _socket.recv_fds(conn, 32, 1)
            conn.close()
            listener.close()

            ret = proc.wait(timeout=5)
            if ret != 0:
                stderr = proc.stderr.read().decode(errors="replace")
                raise RuntimeError(f"open_device_fd_helper.py failed (exit {ret}): {stderr}")
            if not fds:
                raise RuntimeError("helper didn't send an fd")
            self.fd = fds[0]


# Testing a specific hypothesis: two completely different player binaries
# (mpg123, ffmpeg) showed the identical ~8.5s stall periodicity, just at
# different severity -- ruling out either player's own internals as the
# root cause and pointing at something upstream common to both, in this
# process's own loop or the Python runtime. CPython's generational GC
# runs on an allocation-count threshold, not a timer, so a steady-state
# loop allocating at a constant rate (one new bytes object per read here)
# would trigger it at a roughly constant real-time INTERVAL -- exactly
# the kind of "periodic, player-independent stall" signature observed.
# Disabling it is a cheap, directly falsifiable test of that theory.
gc.disable()


def parse_boot_sector(bs):
    bytes_per_sector = int.from_bytes(bs[11:13], "little")
    sectors_per_cluster = bs[13]
    reserved_sectors = int.from_bytes(bs[14:16], "little")
    num_fats = bs[16]
    root_entries = int.from_bytes(bs[17:19], "little")
    total_sectors_16 = int.from_bytes(bs[19:21], "little")
    fat_size_sectors = int.from_bytes(bs[22:24], "little")
    total_sectors_32 = int.from_bytes(bs[32:36], "little")
    volume_serial = int.from_bytes(bs[39:43], "little")
    assert bytes_per_sector == SECTOR_SIZE, f"unexpected sector size {bytes_per_sector}"
    root_dir_lba = reserved_sectors + num_fats * fat_size_sectors
    root_dir_sectors = -(-(root_entries * 32) // bytes_per_sector)
    data_lba = root_dir_lba + root_dir_sectors
    total_sectors = total_sectors_16 if total_sectors_16 else total_sectors_32
    # Real FAT drivers determine FAT12 vs FAT16 vs FAT32 from the DATA
    # CLUSTER COUNT (the actual FAT spec rule), not the informational
    # filesystem-type label string at bytes 54-61 (that field is
    # advisory only, per the spec) -- REAL BUG FOUND live tonight: this
    # reader used to call fat12_entry() unconditionally regardless of
    # which volume it was reading, which silently misinterprets FAT16's
    # flat 16-bit entries as FAT12's 12-bit packed ones, producing
    # garbage cluster-chain values that may never hit an end-of-chain
    # marker -- confirmed to spin forever appending to cluster_list,
    # consuming a car_sim.py process's memory unboundedly (~12.7GB
    # before being killed) against the FAT16 fallback firmware.
    data_clusters = (total_sectors - data_lba) // sectors_per_cluster if sectors_per_cluster else 0
    fat_type = "FAT12" if data_clusters < 4085 else "FAT16"
    return {
        "sectors_per_cluster": sectors_per_cluster,
        "reserved_sectors": reserved_sectors,
        "num_fats": num_fats,
        "root_entries": root_entries,
        "fat_size_sectors": fat_size_sectors,
        "root_dir_lba": root_dir_lba,
        "root_dir_sectors": root_dir_sectors,
        "data_lba": data_lba,
        "fat_type": fat_type,
        "volume_serial": volume_serial,
    }


def find_all_file_entries(root_dir_bytes):
    """Like find_file_entry() but returns EVERY valid entry, not just the first --
    needed for --gui mode's multi-file navigation (testing PLAN_NEXT.md's C3:
    detecting the car radio's own next/prev button presses via multiple
    directory entries that all alias the same live stream)."""
    entries = []
    # VFAT long-name parts seen since the last 8.3 entry: {ordinal: 13 chars},
    # plus the checksum they claim. Radios that show long names read them
    # this way; one that doesn't would just skip attr 0x0F entries.
    lfn_parts, lfn_checksum = {}, None
    for i in range(0, len(root_dir_bytes), 32):
        entry = root_dir_bytes[i:i + 32]
        if entry[0] in (0x00, 0xE5):
            lfn_parts, lfn_checksum = {}, None
            continue
        attr = entry[11]
        if attr == 0x0F:  # long-filename part
            if entry[0] & 0x40:
                lfn_parts, lfn_checksum = {}, entry[13]
            raw = entry[1:11] + entry[14:26] + entry[28:32]
            lfn_parts[entry[0] & 0x1F] = raw.decode("utf-16-le", errors="replace")
            continue
        if attr & 0x08:  # volume label
            lfn_parts, lfn_checksum = {}, None
            continue
        base = entry[0:8].decode("ascii", errors="replace").rstrip()
        ext = entry[8:11].decode("ascii", errors="replace").rstrip()
        name = f"{base}.{ext}" if ext else base
        chk = 0
        for b in entry[0:11]:
            chk = (((chk & 1) << 7) + (chk >> 1) + b) & 0xFF
        if lfn_parts and lfn_checksum == chk and set(lfn_parts) == set(range(1, len(lfn_parts) + 1)):
            long_name = "".join(lfn_parts[k] for k in sorted(lfn_parts))
            name = long_name.split("\x00", 1)[0]
        lfn_parts, lfn_checksum = {}, None
        first_cluster = int.from_bytes(entry[26:28], "little")
        size = int.from_bytes(entry[28:32], "little")
        entries.append({"name": name, "first_cluster": first_cluster, "size": size})
    return entries


def find_file_entry(root_dir_bytes):
    entries = find_all_file_entries(root_dir_bytes)
    return entries[0] if entries else None


def walk_cluster_chain(fat, first_cluster, fat_type):
    """Extracted for --gui mode, which needs to walk MULTIPLE independent
    chains (one per discovered file) instead of just one -- the existing
    single-file path below keeps its own inline walk untouched, so this
    doesn't change any already-tested behavior."""
    cluster_list = []
    c = first_cluster
    eoc = end_of_chain_marker(fat_type)
    while c < eoc and len(cluster_list) <= 65524:
        cluster_list.append(c)
        c = fat_entry(fat, c, fat_type)
    return cluster_list


def fat12_entry(fat_bytes, cluster):
    offset = cluster + cluster // 2
    if cluster % 2 == 0:
        return fat_bytes[offset] | ((fat_bytes[offset + 1] & 0x0F) << 8)
    return (fat_bytes[offset] >> 4) | (fat_bytes[offset + 1] << 4)


def fat16_entry(fat_bytes, cluster):
    offset = cluster * 2
    return int.from_bytes(fat_bytes[offset:offset + 2], "little")


def fat_entry(fat_bytes, cluster, fat_type):
    return fat12_entry(fat_bytes, cluster) if fat_type == "FAT12" else fat16_entry(fat_bytes, cluster)


def end_of_chain_marker(fat_type):
    # FAT12 end-of-chain/reserved values start at 0xFF8; FAT16's start at
    # 0xFFF8 -- a chain walker comparing against the wrong threshold could
    # either stop too early (FAT16 entries 0xFF8-0xFFF7 are ordinary valid
    # cluster numbers, not reserved) or never stop (comparing FAT12
    # entries against 0xFFF8 would treat everything below it as a valid
    # next-cluster, including values that share the FAT12 end-marker
    # range from garbage/uninitialized entries).
    return 0xFF8 if fat_type == "FAT12" else 0xFFF8


RESUME_CACHE_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".car_sim_resume_cache.json")


def load_resume_cache():
    try:
        with open(RESUME_CACHE_PATH) as f:
            return json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        return {}


def resume_cache_key(volume_serial, filename):
    return f"{volume_serial:08x}:{filename}"


def resume_cache_writer_thread(get_snapshot_fn, stop_event):
    """REAL BUG FOUND (2026-09-20, live GUI test): the original
    save_resume_position() below does a full load_resume_cache() (re-reads
    and re-parses the WHOLE JSON file) plus a full re-write, called
    synchronously ONCE PER SECOND from inside --gui mode's real-time-paced
    read loop. That disk I/O's latency/jitter was enough to throw off the
    loop's careful absolute-deadline pacing, causing the reader to drift
    too close to the live write edge and hit straddle-protected zero-fill
    reads constantly -- confirmed live: reintroduced the EXACT same
    "Header missing"/decode-corruption flood already fixed once tonight,
    the moment --gui mode's default resume-cache saving was added. Fixed
    by moving ALL file I/O onto this separate background thread, entirely
    decoupled from the read loop's own timing -- the read loop only ever
    updates a plain in-memory value (get_snapshot_fn), never touches disk
    itself. This thread wakes on its own schedule (a few seconds is more
    than enough resolution for a resume-position cache) and does the
    load+mutate+write here, where jitter can never affect real-time
    playback pacing."""
    while not stop_event.wait(3.0):
        snapshot = get_snapshot_fn()
        if not snapshot:
            continue
        cache = load_resume_cache()
        cache.update(snapshot)
        try:
            with open(RESUME_CACHE_PATH, "w") as f:
                json.dump(cache, f)
        except OSError:
            pass


def save_resume_position(volume_serial, filename, cluster_pos):
    cache = load_resume_cache()
    cache[resume_cache_key(volume_serial, filename)] = cluster_pos
    try:
        with open(RESUME_CACHE_PATH, "w") as f:
            json.dump(cache, f)
    except OSError:
        pass


# ===================== --gui mode (2026-09-19) ==============================
#
# Emulates what the real car radio's own screen + physical next/prev buttons
# would do, to test PLAN_NEXT.md's C3 idea end-to-end: multiple directory
# entries that all alias the same live stream, with the SERVER SIDE (the
# real disk logic in fat_disk_shared.h, built with -DFATDISK_MULTI_FILE)
# detecting which one is being read and relaying a next/prev signal back to
# the real classic ESP32. This file's job is just to be a believable stand-in
# for a real radio's own button-driven file navigation -- it does NOT know
# about or implement any of C3's detection/debounce logic itself, exactly
# like a real radio wouldn't either. Pressing a button here simply switches
# which file this reader is pulling clusters from; everything else (does the
# switch get detected, does it get relayed, does the classic actually send a
# real AVRCP command) is the SERVER's job to get right, which is the whole
# point of testing it this way.


# A real dumb head unit has no independent clock on the audio -- it computes
# "elapsed" the exact same way: bytes read through the current lap of the
# file, divided by the (assumed constant CBR) bitrate. Matches the rate
# already established and used throughout this project's own comments/docs
# (e.g. fat_disk_shared.h's "~16000 B/s" ring-sizing math) -- reusing it here
# keeps this GUI's elapsed display consistent with the real pipeline's actual
# rate rather than guessing a different number.
ASSUMED_BYTES_PER_SEC = 16000

# --bursty read-pattern bound (see gui_read_loop's own comment for the
# full rationale and its bug history -- two earlier batch-based designs
# both had real, confirmed test-tool-only artifacts, not firmware bugs).
# How far ahead of the real-time playback schedule reads are allowed to
# race before pausing to let the schedule catch back up.
#
# REAL BUG FOUND (2026-09-21, live hardware, byte-correlation ground-truth
# test): this used to be 6.0, picked when fat_disk_shared.h's
# LIVE_CATCHUP_THRESHOLD_BYTES was still ~5s. Per-read timing
# instrumentation the same night found reads actually arrive in tight
# pairs ~13ms apart (real I/O latency) rather than the smooth/occasional-
# burst pattern this value was designed around -- each pair races the
# served cursor nearly half a second ahead of the real writer in under
# 15ms of real time, so allowing a full 6s of headroom let the firmware's
# correction mechanism fall badly behind, producing sustained (not just
# occasional) stale audio. LIVE_CATCHUP_THRESHOLD_BYTES was tightened the
# same night (first to ~1s, then to ~0.5s, two clusters) specifically to
# correct for this. This value is set to stay "comfortably above" that
# firmware threshold (same original design intent -- still genuinely
# exercises the correction mechanism, not a no-op), just rescaled to the
# firmware's new, much tighter tolerance instead of the stale 5s figure.
BURST_AHEAD_SECONDS = 1.0

# REAL BUG-CLASS GAP FOUND (2026-09-21): this bench tool passed
# FATDISK_ALWAYS_SERVE_LIVE's design on a real, live GUI test ("nearly 0
# delay") the same night a real car test of the identical firmware hit
# "N/A device" on first connect -- a real, confirmed disk_append()/boot-
# primer bug (see fat_disk_shared.h's own history) leaving a real ~11.9KB
# raw zero-fill gap at the tail of the ring RIGHT AT the mount-time window
# a real radio scans. This tool never caught it because it NEVER performs
# any upfront, whole-file validity check the way a real embedded head
# unit's own mount-time frame-sync/duration-estimate scan plausibly does
# -- it only ever plays sequentially from wherever cluster_pos happens to
# start, so a bad byte range only gets noticed if playback coincidentally
# reads through it at just the right moment. Real MP3-encoded silence
# (Shine, fixed CBR) can legitimately produce long streaks of literal
# 0xFF stuffing bytes (already documented elsewhere in this project) --
# so a run of 0x00 specifically (never legitimate MP3 content, only ever
# produced by an unwritten/memset ring region) is a clean, unambiguous
# signal for exactly this bug class, not confusable with real silence.
ZERO_RUN_ALARM_BYTES = 256  # any single unbroken 0x00 run this long or more is a real red flag
MP3_FRAME_SIZE_APPROX = 418  # matches this project's own documented ~416-420B Shine CBR frame size


def scan_file_validity(transport, layout, file_entry):
    """Reads the file's ENTIRE declared cluster chain once, up front --
    mimicking a plausible real head unit's own mount-time validation (scan
    ahead, build a duration estimate, check frame-sync consistency) rather
    than only ever reading wherever playback happens to start. Reports a
    clear PASS/FAIL-style summary to stderr. Does not block playback --
    the point is deterministic, whole-file diagnostic visibility, the
    exact thing this bench tool was missing when it green-lit a design
    that then failed "N/A device" on the real radio."""
    cluster_list = file_entry["cluster_list"]
    sectors_per_cluster = layout["sectors_per_cluster"]
    print(f"[radio][scan] validity-scanning {file_entry['name']!r} "
          f"({len(cluster_list)} clusters, "
          f"{len(cluster_list) * sectors_per_cluster * SECTOR_SIZE} bytes)...",
          file=sys.stderr)

    total_bytes = 0
    zero_run = 0
    worst_zero_run = 0
    worst_zero_run_offset = 0
    sync_headers_found = 0
    all_bytes = bytearray()
    for cluster in cluster_list:
        lba = layout["data_lba"] + (cluster - 2) * sectors_per_cluster
        data = transport.read(lba, sectors_per_cluster)
        all_bytes.extend(data)
        for b in data:
            if b == 0x00:
                zero_run += 1
                if zero_run > worst_zero_run:
                    worst_zero_run = zero_run
                    worst_zero_run_offset = total_bytes - zero_run + 1
            else:
                zero_run = 0
            total_bytes += 1

    # Simple MP3 frame-sync density check: count byte positions matching
    # the 11-bit frame sync pattern (0xFF followed by a byte with its top
    # 3 bits set). Real, valid CBR MP3 content has one roughly every
    # MP3_FRAME_SIZE_APPROX bytes; a corrupted/zero-filled stretch has
    # none at all.
    for i in range(len(all_bytes) - 1):
        if all_bytes[i] == 0xFF and (all_bytes[i + 1] & 0xE0) == 0xE0:
            sync_headers_found += 1
    expected_sync_headers = max(1, total_bytes // MP3_FRAME_SIZE_APPROX)
    sync_density_pct = 100.0 * sync_headers_found / expected_sync_headers

    problems = []
    if worst_zero_run >= ZERO_RUN_ALARM_BYTES:
        problems.append(
            f"raw zero-fill run of {worst_zero_run} bytes at offset {worst_zero_run_offset} "
            f"(~{worst_zero_run_offset / ASSUMED_BYTES_PER_SEC:.2f}s into the file) -- "
            f"never-written ring content, not real encoded silence")
    if sync_density_pct < 50.0:
        problems.append(
            f"MP3 frame-sync density only {sync_density_pct:.1f}% of expected "
            f"({sync_headers_found}/{expected_sync_headers}) -- large stretches with no "
            f"valid frame headers at all")

    if problems:
        print(f"[radio][scan] FAIL -- {file_entry['name']!r} has real, structural "
              f"validity problems a real head unit's own mount-time scan could plausibly "
              f"reject as \"invalid file\":", file=sys.stderr)
        for p in problems:
            print(f"[radio][scan]   - {p}", file=sys.stderr)
    else:
        print(f"[radio][scan] PASS -- {file_entry['name']!r}: sync density "
              f"{sync_density_pct:.1f}%, worst zero-run {worst_zero_run}B "
              f"(threshold {ZERO_RUN_ALARM_BYTES}B)", file=sys.stderr)
    return not problems


class RadioGuiState:
    def __init__(self, files):
        self.files = files  # list of {"name": str, "cluster_list": [int, ...]}
        self.current_index = 0
        self.requested_index = 0
        self.total_read = 0
        self.lap_bytes = 0  # bytes read since the current file last started from position 0
        self.reader_alive = True
        self.paused = False  # Muni's request (2026-09-22): a real Pause/Resume button
        self._lock = threading.Lock()
        # Plain in-memory dict, updated by the read loop with zero I/O --
        # resume_cache_writer_thread() is the only thing that ever touches
        # disk for this, on its own separate schedule. See gui_read_loop's
        # own comment for why this split exists (a real, confirmed timing
        # bug from doing the file I/O directly in the real-time read loop).
        self.pending_resume_positions = {}

    def request_switch(self, delta):
        with self._lock:
            self.requested_index = (self.requested_index + delta) % len(self.files)

    def maybe_apply_switch(self):
        """Called from the reader thread only. Returns True if a switch was
        just applied (caller should reset its own cluster_pos to 0)."""
        with self._lock:
            if self.requested_index != self.current_index:
                self.current_index = self.requested_index
                return True
            return False

    def current_name(self):
        with self._lock:
            return self.files[self.current_index]["name"]

    def toggle_paused(self):
        with self._lock:
            self.paused = not self.paused
            return self.paused

    def is_paused(self):
        with self._lock:
            return self.paused

    def note_resume_position(self, key, cluster_pos):
        with self._lock:
            self.pending_resume_positions[key] = cluster_pos

    def snapshot_resume_positions(self):
        with self._lock:
            return dict(self.pending_resume_positions)

    def snapshot(self):
        with self._lock:
            return (self.current_index, len(self.files), self.total_read,
                    self.lap_bytes, self.reader_alive, self.paused)


def gui_read_loop(transport, layout, state, player, capture_f, volume_serial, bursty_pacing=False, level_proc=None):
    """Same read-decode-loop shape as main()'s existing single-file loop
    below, generalized to react to state.requested_index changing (a button
    press) by switching which file's cluster chain it's pulling from, always
    restarting the newly-selected file at its own beginning -- matching how
    a real radio presents a freshly-selected track.

    REAL BUG FOUND AND FIXED (2026-09-20, live GUI test): unlike the
    single-file loop below, this one needs its own pacing. The single-file
    loop's module docstring explains why pacing was deliberately REMOVED
    from THIS FILE once already -- an earlier version's fixed bitrate
    constant was never quite right and caused a slow-building drift bug over
    long sessions. That reasoning doesn't carry over cleanly here: this
    reader can hit a real straddle-protected zero-fill read (already known,
    already-documented elsewhere in this project to happen at a low but
    nonzero rate -- see fat_disk_shared.h's own READ_MARGIN_BYTES history),
    which can desync ffmpeg's decoder. Once desynced, ffmpeg has nothing
    valid queued for its real-time (-f pulse) output to actually pace
    against, so it stops applying real-time backpressure on stdin -- and
    with NO pacing of its own, this loop then races through the entire
    ~4-minute file in a few seconds (confirmed live: reads=987 in ~6s
    real time, vs. a real ~240s lap at the pipeline's actual encode rate).
    That's exactly what "just jumping around" sounds like: hearing many
    different points of the file in rapid succession. Fixed with an
    ABSOLUTE-DEADLINE pace (anchored once to a fixed start time, re-derived
    fresh every iteration -- not a per-iteration sleep, which is what
    actually causes compounding drift) so a hiccup can never let the reader
    burst arbitrarily far ahead of real time, whether or not ffmpeg's own
    backpressure is currently working.

    REAL DIVERGENCE FROM THE ACTUAL CAR RADIO FOUND (2026-09-20, live
    bench test): this loop always restarted every file at cluster_pos=0 on
    launch. On a ring that happens to have a long stretch of stale/injected
    silence sitting at the start (confirmed directly tonight: ~46s of
    silence-encoded content at the very start of the ring, left over from
    an earlier Bluetooth-debugging outage), that produces a real, long
    silent delay before any audio -- but Muni's real, repeated experience
    with the ACTUAL Kenwood radio is a consistent ~10s regardless. This
    project's own code already theorized why (see `--simulate-radio-resume-
    cache`'s own comment, added after a real hardware test where the radio
    appeared to resume mid-file rather than always restarting cold): a real
    head unit plausibly caches a resume position keyed on volume identity +
    filename, and since the S3's volume serial only changes when the S3
    ITSELF reboots (not on every classic reflash, not on every GUI
    restart), a real radio reconnecting within the same S3 session would
    resume from near wherever it last left off -- never revisiting stale
    early-ring content. That mechanism existed in this file's OLDER
    single-file/TCP-transport code path (behind an opt-in
    `--simulate-radio-resume-cache` flag) but was never carried over when
    --gui/DeviceTransport was added tonight. Fixed by making the SAME
    resume-cache mechanism the DEFAULT (not opt-in) behavior for --gui mode
    specifically, since the whole point of this tool is to accurately
    emulate the real radio's real behavior, not a strictly-more-pessimistic
    always-cold-restart approximation of it.
    """
    cluster_bytes = layout["sectors_per_cluster"] * SECTOR_SIZE

    def open_current_file():
        # A real head unit's FAT layer (e.g. FatFs f_open) reads the file's
        # directory entry when it OPENS the file -- the name it displays and
        # the size it stops at come from that read, not from whatever the
        # directory said at mount time. Matched by first cluster, since the
        # S3 rewrites names at runtime (TITLE: -> set_file_name()).
        f = state.files[state.current_index]
        root = transport.read(layout["root_dir_lba"], layout["root_dir_sectors"])
        for e in find_all_file_entries(root):
            if e["first_cluster"] == f["first_cluster"]:
                f["name"] = e["name"]
                f["size"] = e["size"]
                break
        return f["size"]

    # Thread start = drive (re)inserted: resume position applies. Every
    # later open (Next/Back/EOF) starts the new file at byte 0, like a real
    # radio selecting a track.
    cluster_pos = state.files[state.current_index].get("start_cluster_pos", 0)
    file_size = open_current_file()
    file_bytes = cluster_pos * cluster_bytes
    # REAL BUG FOUND (2026-09-20, live test against FATDISK_ALWAYS_SERVE_LIVE
    # real hardware): this used to compute sleep_time against a FIXED
    # start_time/bytes_since_start baseline that never re-anchors. If this
    # loop EVER falls behind schedule even once (ordinary OS/Python
    # scheduling jitter, unavoidable over a real run), sleep_time goes
    # negative and STAYS negative forever after -- with no correction, this
    # loop then reads strictly faster than real-time for the rest of the
    # session, permanently. Against the ORIGINAL, offset-based S3 design
    # this was survivable (just meant occasionally overtaking the writer,
    # producing a straddle-protected zero-fill once in a while). Against
    # FATDISK_ALWAYS_SERVE_LIVE it's much worse: reading faster than the
    # real encode rate means this loop's own persistent server-side cursor
    # keeps chasing content that hasn't been written yet, producing
    # constant misalignment -- confirmed live: recurring "Illegal Audio-
    # MPEG-Header" resyncs roughly every 8192 bytes. tcp_paced_listen.py
    # (used to validate the new design before this bug was found) already
    # had the correct fix for this same class of pacing loop -- carrying it
    # over here: when the loop is behind, just re-anchor the schedule to
    # "now" instead of trying to catch up to an increasingly-unrealistic
    # original baseline.
    #
    # REAL GAP FOUND (2026-09-21, after a real car test failed despite this
    # tool passing): this smooth, continuously-paced-to-exactly-16000B/s
    # loop is STRUCTURALLY INCAPABLE of exercising a real firmware bug found
    # the same night -- the live-serve cursor's "reader has raced ahead of
    # the writer" correction path (fat_disk_shared.h's LIVE_CATCHUP_
    # THRESHOLD_BYTES ahead-direction fix) only ever fires if the reader
    # actually reads FASTER than real-time, which this loop was specifically
    # built never to do. A real embedded USB-MSC host/decoder very plausibly
    # reads ahead into a bounded internal buffer rather than at a smooth
    # per-byte rate -- this tool testing ONLY the smooth pattern gave false
    # confidence that a design was ready for real hardware when it hadn't
    # actually been exercised the way the real target very plausibly
    # behaves.
    #
    # TWO REAL BUGS FOUND AND FIXED IN EARLIER VERSIONS OF bursty_pacing
    # (2026-09-21, both found via live tests against real hardware, both
    # confirmed via the S3's own FATDISK_LIVE_DEBUG trace before concluding
    # anything about the firmware): the first version read a large batch of
    # clusters instantly then wrote them ALL to the player immediately too
    # -- followed by one long silent sleep -- causing the player to run dry
    # mid-burst and audibly stall ("plays 1-2s, stops, comes back"). The
    # SECOND version fixed that by draining a burst's already-read data to
    # the player with per-cluster pacing -- but STILL read the entire burst
    # in one upfront batch with ZERO new reads issued during the whole
    # drain phase. The S3's own debug trace proved this was still wrong:
    # g_write_pos barely moved during the fast-read burst (confirming reads
    # ARE genuinely bursty, as intended), but the cursor's "ahead" amount
    # kept COMPOUNDING across successive burst+drain cycles -- because with
    # zero reads happening during each ~5-6s drain phase, nothing ever lets
    # accumulated ahead-drift shrink back down; each new burst's fast-reads
    # just add MORE drift on top of whatever was already there from the
    # last cycle. This doesn't match how a real decoder's bounded read-
    # ahead buffer actually behaves: a real buffer is continuously topped
    # up as it drains, never goes fully silent on the read side for
    # multiple seconds at a stretch. The firmware's own jump-correction
    # was independently confirmed CORRECT from the same trace (lands the
    # cursor at exactly safe_edge - n_safe, precisely as designed) -- the
    # compounding drift was entirely this test tool's own artifact.
    #
    # CORRECTED MODEL: a genuine bounded read-ahead allowance, not a batch.
    # Reads are issued with NO sleep as long as the reader is within
    # BURST_AHEAD_SECONDS of the real-time playback schedule; once reading
    # would push it further ahead than that, sleep just enough to bring it
    # back to the allowance boundary before the next read. Every cluster is
    # written to the player IMMEDIATELY after being read (never batched),
    # so delivery is always continuous -- only the READ REQUESTS burst
    # ahead, exactly matching what a real bounded-buffer decoder's read
    # side would do, without ever silently starving the player or letting
    # drift compound unchecked across cycles.
    next_send_time = time.monotonic()
    loop_start_time = next_send_time
    # REAL BUG FOUND (2026-09-21, live test against real hardware with a
    # real player attached): a cold-started bursty reader had the FULL
    # BURST_AHEAD_SECONDS allowance available from its very first read,
    # since `ahead` starts at 0 and each read is cheap (a few ms of real
    # I/O) while next_send_time jumps forward a full cluster's worth of
    # assumed playback time -- so `ahead` climbed from 0 to the 6s ceiling
    # in well under a second of real wall-clock time. Against
    # FATDISK_ALWAYS_SERVE_LIVE, "reading ahead" doesn't fetch real future
    # content (the server ignores requested LBA and always serves from its
    # own live cursor) -- it just races that persistent cursor forward past
    # what the writer has actually produced in that same instant, forcing
    # 1-2 jump-corrections bunched together right at connect (confirmed via
    # direct reproduction: exactly 2 "Illegal Audio-MPEG-Header" events,
    # both within the first ~9KB, then clean for the rest of a 60s/~1MB
    # run). Fixed by ramping the allowance up from 0 over the first
    # BURST_AHEAD_SECONDS of REAL elapsed time instead of granting the full
    # allowance instantly -- steady-state behavior after the ramp period is
    # unchanged, so this doesn't weaken what --bursty is actually testing.
    was_paused = False
    try:
        while True:
            if state.is_paused():
                # Muni's request (2026-09-22): a real Pause/Resume button.
                # Just stop requesting/feeding new bytes -- the player's own
                # small buffer drains and it goes silent naturally, exactly
                # like a real radio's pause would. Resuming continues
                # reading forward from the SAME cluster_pos (whatever
                # content played during the pause is simply skipped, never
                # played -- correct for a live continuous stream, same as a
                # real radio pausing/resuming live BT audio).
                was_paused = True
                time.sleep(0.1)
                continue
            if was_paused:
                # Re-anchor pacing to right now instead of trying to "catch
                # up" to however far real wall-clock drifted during the
                # pause -- next_send_time is an absolute deadline (see this
                # function's own docstring on why), so without this reset,
                # resuming would burst-read through many clusters at once
                # to make up the paused interval.
                next_send_time = time.monotonic()
                was_paused = False
            if state.maybe_apply_switch():
                cluster_pos = 0
                file_size = open_current_file()
                file_bytes = 0
                state.lap_bytes = 0
            if file_bytes >= file_size:
                # EOF at the declared size (not the cluster chain's end): a
                # real radio moves on to the next file in directory order.
                if len(state.files) > 1:
                    state.request_switch(1)
                else:
                    cluster_pos = 0
                    file_size = open_current_file()
                    file_bytes = 0
                    state.lap_bytes = 0
                if file_size == 0:
                    time.sleep(0.1)
                continue
            cluster_list = state.files[state.current_index]["cluster_list"]
            cluster = cluster_list[cluster_pos]
            lba = layout["data_lba"] + (cluster - 2) * layout["sectors_per_cluster"]
            if bursty_pacing:
                # Only sleep if reading now would push the schedule further
                # ahead than the allowed read-ahead buffer -- otherwise
                # read immediately (racing ahead, up to the bound).
                now = time.monotonic()
                allowed_ahead = min(BURST_AHEAD_SECONDS, now - loop_start_time)
                ahead = next_send_time - now
                if ahead > allowed_ahead:
                    time.sleep(ahead - allowed_ahead)
            # REAL BUG FOUND (2026-09-22, Muni: unplugged the real S3 mid-
            # test, GUI kept showing "Now Playing" with bytes read still
            # climbing forever). Root cause: os.preadv() against a real
            # block-device fd does NOT reliably raise once the underlying
            # USB device is physically removed -- confirmed live, the fd
            # (already pointing at "/dev/sda (deleted)" per /proc) kept
            # returning data with zero errors for hours after unplug, so
            # the existing `except (OSError, ConnectionError)` below never
            # fired and the GUI never learned playback had become phantom.
            # The one thing that DOES reliably reflect removal is the
            # device node itself disappearing from the filesystem -- a
            # cheap os.path.exists() stat catches that even though the
            # read() syscall itself won't.
            device_path = getattr(transport, "path", None)
            if device_path is not None and not os.path.exists(device_path):
                raise OSError(f"device node {device_path} no longer exists "
                               f"(physically unplugged)")
            data = transport.read(lba, layout["sectors_per_cluster"])
            if len(data) > file_size - file_bytes:
                data = data[:file_size - file_bytes]
            file_bytes += len(data)
            if capture_f:
                capture_f.write(data)
            if player:
                try:
                    player.stdin.write(data)
                    player.stdin.flush()
                except (BrokenPipeError, ValueError):
                    # ValueError: the window was closed (on_close() closed the
                    # player's stdin) while this thread was mid-write.
                    print("[radio] player exited, stopping", file=sys.stderr)
                    break
            if level_proc is not None:
                # Best-effort, non-blocking duplicate for the silence/level
                # meter -- level_proc.stdin's fd was set non-blocking at
                # construction specifically so this can NEVER delay real
                # playback above. A full pipe or a dead process just drops
                # this chunk (the meter reading goes briefly stale, nothing
                # else) instead of ever stalling the loop that feeds the
                # actual audio player. Uses a raw os.write() on the fd
                # directly rather than the stdin file object's own
                # .write() -- REAL BUG FOUND (2026-09-24, live test): the
                # file object is a BufferedWriter with its own internal
                # buffering/retry logic layered on top of the fd, which
                # doesn't reliably surface a non-blocking fd's real EAGAIN
                # behavior -- confirmed live, level_proc received literally
                # zero bytes for 10+ real seconds through .write() despite
                # never raising an exception. A raw os.write() talks to the
                # non-blocking fd directly, matching what os.set_blocking()
                # actually configured.
                try:
                    os.write(level_proc.stdin.fileno(), data)
                except (BrokenPipeError, BlockingIOError, OSError):
                    pass
            next_send_time += len(data) / ASSUMED_BYTES_PER_SEC
            if not bursty_pacing:
                sleep_time = next_send_time - time.monotonic()
                if sleep_time > 0:
                    time.sleep(sleep_time)
                else:
                    next_send_time = time.monotonic()
            state.total_read += len(data)
            state.lap_bytes += len(data)
            cluster_pos = (cluster_pos + 1) % len(cluster_list)
            if cluster_pos == 0:
                # Chain ran out before the declared size (malformed volume):
                # treat as EOF; the check at the top of the loop advances.
                file_bytes = file_size
            # Zero disk I/O -- a plain, lock-protected dict write, cheap
            # enough every iteration. See resume_cache_writer_thread() for
            # where this actually reaches disk, on its own decoupled
            # schedule.
            name = state.files[state.current_index]["name"]
            state.note_resume_position(resume_cache_key(volume_serial, name), cluster_pos)
    except (OSError, ConnectionError) as e:
        print(f"[radio][gui] socket error, reader thread stopping: {e}", file=sys.stderr)
    finally:
        # Persist exactly where this thread was, keyed the same way the
        # thread's own startup reads it back (line 646 above) -- so a
        # reconnect_supervisor() restart (see run_gui_mode) picks up right
        # where this thread left off instead of jumping back to wherever
        # playback happened to be when the GUI first launched. Needed
        # because note_resume_position()'s own disk-backed cache is keyed
        # by (volume_serial, filename) and only flushed on its own ~3s
        # schedule -- this in-memory write is immediate and exact, no race.
        state.files[state.current_index]["start_cluster_pos"] = cluster_pos
        state.reader_alive = False


def run_gui_mode(transport, layout, fat, root_dir, args):
    # Lazy import: --gui is opt-in, so a headless box without tkinter
    # installed can still use every other mode in this file unaffected.
    import tkinter as tk

    entries = find_all_file_entries(root_dir)
    if not entries:
        print("[radio][gui] no files found in root directory, aborting", file=sys.stderr)
        return
    volume_serial = layout["volume_serial"]
    resume_cache = load_resume_cache()
    files = []
    for e in entries:
        cluster_list = walk_cluster_chain(fat, e["first_cluster"], layout["fat_type"])
        if not cluster_list:
            print(f"[radio][gui] file {e['name']!r} has an empty cluster chain, skipping",
                  file=sys.stderr)
            continue
        # Default (not opt-in) resume-cache behavior for --gui, matching the
        # real Kenwood radio's own apparent behavior -- see gui_read_loop's
        # own comment for the full reasoning. Falls back to 0 (cold start)
        # on a genuinely fresh volume identity (S3 rebooted) or first run.
        key = resume_cache_key(volume_serial, e["name"])
        cached_pos = resume_cache.get(key)
        start_cluster_pos = cached_pos if (cached_pos is not None and cached_pos < len(cluster_list)) else 0
        if start_cluster_pos:
            print(f"[radio][gui] resume cache hit for {key} -- starting {e['name']!r} "
                  f"at cluster_pos={start_cluster_pos} instead of 0", file=sys.stderr)
        files.append({"name": e["name"], "cluster_list": cluster_list,
                      "start_cluster_pos": start_cluster_pos,
                      "first_cluster": e["first_cluster"], "size": e["size"]})
    if not files:
        print("[radio][gui] no playable files found, aborting", file=sys.stderr)
        return
    print(f"[radio][gui] found {len(files)} file(s): {[f['name'] for f in files]}",
          file=sys.stderr)

    # Mount-time validity scan (2026-09-21) -- see scan_file_validity()'s own
    # comment for why this exists: this tool previously never caught a real,
    # confirmed firmware bug (a boot-primer tail-gap leaving raw zero-fill at
    # the ring's physical tail) precisely because it only ever played
    # sequentially from wherever, never validating the WHOLE declared file
    # up front the way a real head unit's own mount-time scan plausibly
    # does. Runs for every discovered file, not just the one about to play.
    #
    # REAL BUG FOUND AND FIXED (2026-09-21, live test, reproduced directly):
    # running this scan immediately before --bursty playback under
    # FATDISK_ALWAYS_SERVE_LIVE produces real, clustered audio corruption
    # ("Frankenstein stream" warnings, multiple resyncs) -- confirmed via a
    # direct A/B (scan+bursty vs. skip-scan+bursty, otherwise identical)
    # against real hardware. Root cause: the scan itself does 938 UNPACED
    # reads in a tight loop -- a far more extreme, unbounded version of
    # exactly what --bursty does in a controlled way (BURST_AHEAD_SECONDS-
    # bounded), and it advances the SAME shared, persistent live-serve
    # cursor the real playback read loop depends on. By the time the scan
    # finishes, the cursor is left in a state the subsequent --bursty
    # playback has never been tuned to recover cleanly from. The scan
    # itself still correctly reports PASS (it does its own job right) --
    # the harm is a SIDE EFFECT on cursor state, not a scan-accuracy bug.
    # Fixed by never running the two together: the scan's real, proven
    # value (catching the B2 boot-primer tail-gap bug) doesn't require
    # --bursty pacing at all, so skip it automatically whenever --bursty is
    # requested rather than let them silently corrupt each other.
    if args.bursty and not args.skip_scan:
        print("[radio][gui] skipping validity scan -- incompatible with --bursty "
              "(the scan's own unpaced full-file read perturbs the same live-serve "
              "cursor --bursty playback depends on; confirmed live, 2026-09-21). "
              "Run once WITHOUT --bursty first if you want scan coverage this session.",
              file=sys.stderr)
    elif not args.skip_scan:
        for f in files:
            scan_file_validity(transport, layout, f)

    state = RadioGuiState(files)
    # Scriptable Next/Back for unattended tests (`kill -USR1 <pid>` = Next,
    # `-USR2` = Back): same request_switch() path as the buttons. Tkinter's
    # periodic refresh() keeps the interpreter running Python code, so the
    # handlers fire within ~200ms.
    import signal
    signal.signal(signal.SIGUSR1, lambda *_: state.request_switch(1))
    signal.signal(signal.SIGUSR2, lambda *_: state.request_switch(-1))
    player = None
    level_proc = None  # only set for the default ffmpeg player, see below
    capture_f = open(args.capture, "wb") if args.capture else None
    if not args.no_play:
        if args.player == "mpg123":
            # See real_s3_listen.py's own header comment for why this
            # exists as an option: ffmpeg's ultra-low-latency config
            # (below) cannot cleanly resync after a real content
            # discontinuity in this ring's stream -- a real, already-
            # confirmed limitation, not specific to any one design. This
            # matters MORE for FATDISK_ALWAYS_SERVE_LIVE (which
            # deliberately introduces a jump on cursor-catch-up) than for
            # the original design, so use this player when testing that.
            #
            # CAUTION (2026-09-21): --resync-limit -1 is unlimited resync
            # tolerance -- a real "does this design survive at all" test
            # against THIS player alone is NOT evidence it'll survive the
            # real car radio's own, almost-certainly-far-less-tolerant
            # embedded decoder. Confirmed the hard way: this exact
            # configuration reported "nearly 0 delay" success the same
            # night a real car test of the identical firmware failed
            # outright. Use --player mpg123-strict for a materially more
            # realistic decoder-tolerance bar before trusting a "works" result.
            player = subprocess.Popen(["mpg123", "--resync-limit", "-1", "-"],
                                       stdin=subprocess.PIPE)
        elif args.player == "mpg123-strict":
            # mpg123's own small DEFAULT resync window (no --resync-limit
            # override) -- see this flag's own --help text above for why
            # this exists. Closer to what a real, cheap embedded decoder's
            # actual error tolerance is likely to look like than the
            # unlimited-tolerance mode above.
            player = subprocess.Popen(["mpg123", "-"], stdin=subprocess.PIPE)
        else:
            # -af astats(...)+ametadata(print) (2026-09-22, Muni: "make it
            # also say if its silence or not or the volume"): astats with
            # reset=1 recomputes RMS level fresh every audio frame instead
            # of accumulating over the whole stream, and ametadata prints
            # that per-frame value to this process's own stdout as plain
            # text -- picked over ffmpeg's volumedetect filter, which only
            # ever reports one summary number at end-of-stream, useless for
            # a live "is it silent RIGHT NOW" readout. file=- means stdout,
            # a separate fd from the actual decoded audio (which goes out
            # via the real -f pulse device, not through this process's
            # stdout at all) -- doesn't interfere with playback.
            player = subprocess.Popen(
                ["chrt", "--rr", "50", "ffmpeg", "-loglevel", "warning",
                 "-thread_queue_size", "4096",
                 "-fflags", "nobuffer", "-flags", "low_delay",
                 "-analyzeduration", "0", "-probesize", "4096", "-f", "mp3",
                 "-i", "-", "-f", "pulse",
                 "-name", "car_sim_radio_player",
                 "-prebuf", "0", "-buffer_duration", "50", "default"],
                stdin=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

            # REAL BUG FOUND AND FIXED (2026-09-24, Muni: massive new
            # playback delay after the silence/volume readout was added --
            # "we werent even touching [ring size]... the delay was massive
            # now, and it wasnt before"): the astats/ametadata filter used
            # to live INLINE in this SAME ffmpeg process's -af chain, ahead
            # of the real -f pulse output. That's a single synchronous
            # filter graph -- if this process's own stdout pipe (feeding
            # ametadata's print target) ever backed up even briefly (e.g.
            # this Python process's stdout_watcher thread not getting
            # scheduled promptly), ffmpeg's write() to that pipe blocks,
            # and since it's the same filter graph, that stalls the actual
            # decoded audio reaching -f pulse too -- not just the metadata.
            # Fixed by moving the level metering to a fully SEPARATE ffmpeg
            # process (level_proc below), fed a best-effort, non-blocking
            # DUPLICATE of the same bytes -- if this second process's pipe
            # ever backs up, that write is simply skipped (see
            # gui_read_loop's own write site), so it can never delay the
            # real playback process by even one byte, regardless of how
            # slowly (or not at all) this side is drained.
            level_proc = subprocess.Popen(
                ["ffmpeg", "-loglevel", "warning",
                 "-thread_queue_size", "4096",
                 "-fflags", "nobuffer", "-flags", "low_delay",
                 "-analyzeduration", "0", "-probesize", "4096", "-f", "mp3",
                 "-i", "-",
                 "-af", "astats=metadata=1:reset=1,"
                        "ametadata=mode=print:key=lavfi.astats.Overall.RMS_level:file=-:direct=1",
                 "-f", "null", "-"],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
            )
            os.set_blocking(level_proc.stdin.fileno(), False)

    # Surface decode errors visibly in the GUI (2026-09-22, Muni's request):
    # "if the gui hits invalid mp3 data, that it errors as well, cuz the car
    # radio does" -- a real car radio's decoder would audibly/visibly react
    # to bad data, but ffmpeg's own decode errors (confirmed real this
    # session: "Header missing", "invalid block type", "big_values too
    # big", right after a live-serve cursor jump) previously only went to
    # this process's own stderr -- invisible unless someone went digging
    # through a log file. A dedicated thread watches the player's stderr
    # for these specific error signatures and records when they happen;
    # the GUI's own periodic refresh() below (already polling every 200ms)
    # surfaces a visible warning while errors are recent, clearing once
    # they stop.
    decode_error_state = {"count": 0, "last_ms": 0.0}
    DECODE_ERROR_PATTERNS = ("Header missing", "invalid block type",
                              "big_values too big", "Error submitting packet",
                              "Error while decoding")

    # REAL TOOLING GAP FOUND AND FIXED (2026-09-22): ffmpeg's stderr is
    # captured via subprocess.PIPE, read ONLY by this thread, and previously
    # only ever fed into the in-memory decode_error_state counter -- never
    # written anywhere externally checkable. That meant the on-screen count
    # could climb into the thousands with zero way to verify from outside
    # the GUI process whether that was a real ONGOING problem or stale
    # accumulation from much earlier in a long-running session (e.g.
    # across several unrelated classic-board reflashes/reconnects). Now
    # also appends every matched error line, with a wall-clock timestamp,
    # to a real log file -- lets an external check distinguish "steady
    # trickle over an hour" from "actively glitching right now."
    error_log_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                   "..", "logs", "gui_decode_errors.log")
    os.makedirs(os.path.dirname(error_log_path), exist_ok=True)
    error_log_f = open(error_log_path, "a", buffering=1)
    error_log_f.write(f"\n=== GUI decode-error log (re)started {time.strftime('%Y-%m-%d %H:%M:%S')} ===\n")

    def stderr_watcher():
        if player is None or player.stderr is None:
            return
        for raw_line in iter(player.stderr.readline, b""):
            line = raw_line.decode("utf-8", errors="replace")
            if any(pat in line for pat in DECODE_ERROR_PATTERNS):
                decode_error_state["count"] += 1
                decode_error_state["last_ms"] = time.monotonic()
                error_log_f.write(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {line.rstrip()}\n")

    if player is not None and player.stderr is not None:
        threading.Thread(target=stderr_watcher, daemon=True).start()

    # Muni's request (2026-09-22): "make it also say if its silence or not
    # or the volume" -- reads the per-frame RMS level ffmpeg's own -af
    # astats+ametadata filter (added above, default ffmpeg player only)
    # prints to its stdout. SILENCE_THRESHOLD_DB matches the classic
    # ESP32's own silence-injection intent (there's no real signal to
    # measure below this, just noise floor/injected silence) -- not tied
    # to any single number elsewhere in this project, chosen as a
    # reasonable "clearly not real audio" cutoff.
    SILENCE_THRESHOLD_DB = -50.0
    audio_level_state = {"rms_db": None, "last_ms": 0.0}
    RMS_LINE_RE = re.compile(r"lavfi\.astats\.Overall\.RMS_level=(-?[\d.]+|-?inf)")

    def stdout_watcher():
        if level_proc is None or level_proc.stdout is None:
            return
        for raw_line in iter(level_proc.stdout.readline, b""):
            line = raw_line.decode("utf-8", errors="replace")
            m = RMS_LINE_RE.search(line)
            if m:
                val = m.group(1)
                # ffmpeg reports literal "-inf" for a frame of exact digital
                # silence (all-zero samples) -- not a float, and correctly
                # the quietest possible reading, not a parse error.
                audio_level_state["rms_db"] = float("-inf") if "inf" in val else float(val)
                audio_level_state["last_ms"] = time.monotonic()

    if level_proc is not None and level_proc.stdout is not None:
        threading.Thread(target=stdout_watcher, daemon=True).start()

    reader_thread = threading.Thread(
        target=gui_read_loop,
        args=(transport, layout, state, player, capture_f, volume_serial, args.bursty),
        kwargs={"level_proc": level_proc},
        daemon=True)
    reader_thread.start()

    # Muni's request (2026-09-22, live bench test): unplugging/replugging
    # the real S3's native USB-OTG port didn't auto-resume -- the reader
    # thread correctly detected the device was gone (the os.path.exists()
    # liveness check added earlier tonight, since a removed device's
    # already-open fd doesn't reliably error on read()) but nothing ever
    # tried reconnecting once it came back; the GUI just sat there showing
    # "[reader stopped]" forever until manually relaunched. Only meaningful
    # for real hardware (--device), never TCP mode -- getattr(transport,
    # "path", None) is the same test gui_read_loop's own liveness check
    # uses, so this only activates when there's actually a device path to
    # recheck.
    reconnect_stop = threading.Event()

    def reconnect_supervisor():
        while not reconnect_stop.is_set():
            if not state.reader_alive:
                print("[radio][gui] device link down -- waiting for it to "
                      "come back...", file=sys.stderr)
                new_transport = None
                while not reconnect_stop.is_set() and new_transport is None:
                    # Re-run find_s3_block_device() on every attempt if
                    # --device auto was used -- a re-enumerated device can
                    # land on a different /dev/sdX path (confirmed live
                    # tonight: sda -> sdb across one unplug/replug cycle),
                    # so rechecking the OLD fixed path would never notice
                    # it came back under a new name.
                    device_path = find_s3_block_device() if args.device_was_auto else args.device
                    if device_path is not None and os.path.exists(device_path):
                        try:
                            new_transport = HelperDeviceTransport(device_path, SUDO_PASSWORD)
                        except Exception as e:
                            print(f"[radio][gui] reconnect attempt failed: {e}", file=sys.stderr)
                    if new_transport is None:
                        reconnect_stop.wait(1.0)
                if reconnect_stop.is_set():
                    break
                print("[radio][gui] device back -- resuming playback", file=sys.stderr)
                state.reader_alive = True
                threading.Thread(
                    target=gui_read_loop,
                    args=(new_transport, layout, state, player, capture_f,
                          volume_serial, args.bursty),
                    kwargs={"level_proc": level_proc},
                    daemon=True).start()
            reconnect_stop.wait(0.5)

    if getattr(transport, "path", None) is not None:
        threading.Thread(target=reconnect_supervisor, daemon=True).start()

    # Snapshots (not pops) so the writer always has the latest known
    # position for every file touched so far, even across its own 3s gaps.
    writer_stop = threading.Event()
    writer_thread = threading.Thread(
        target=resume_cache_writer_thread,
        args=(state.snapshot_resume_positions, writer_stop),
        daemon=True)
    writer_thread.start()

    root = tk.Tk()
    root.title("Car Radio Simulator")
    root.geometry("420x260")

    now_playing_var = tk.StringVar(value="Now Playing: --")
    elapsed_var = tk.StringVar(value="0:00")
    status_var = tk.StringVar(value="")

    tk.Label(root, textvariable=now_playing_var, font=("Sans", 18, "bold")).pack(pady=(24, 4))
    tk.Label(root, textvariable=elapsed_var, font=("Sans", 22)).pack(pady=(0, 4))
    tk.Label(root, textvariable=status_var, font=("Sans", 10)).pack(pady=(0, 14))

    btn_frame = tk.Frame(root)
    btn_frame.pack()
    tk.Button(btn_frame, text="◀ Back", width=12,
              command=lambda: state.request_switch(-1)).grid(row=0, column=0, padx=10)
    tk.Button(btn_frame, text="Next ▶", width=12,
              command=lambda: state.request_switch(1)).grid(row=0, column=1, padx=10)

    def toggle_pause():
        is_paused = state.toggle_paused()
        pause_btn.config(text="▶ Resume" if is_paused else "⏸ Pause")

    pause_btn = tk.Button(btn_frame, text="⏸ Pause", width=12, command=toggle_pause)
    pause_btn.grid(row=0, column=2, padx=10)

    # Local-only playback volume (2026-09-22, Muni's request): adjusts just
    # this PC's own PulseAudio playback of the ffmpeg player process below --
    # never touches the classic ESP32 or the S3 in any way, purely a
    # convenience for bench-listening at a comfortable level. Only wired up
    # for the default ffmpeg player, which is given a distinct PulseAudio
    # stream name ("car_sim_radio_player", see its spawn args above)
    # specifically so it can be targeted without affecting any other audio
    # on the machine. mpg123/mpg123-strict don't get a distinct named
    # stream, so the slider is a no-op (with a clear status message) there.
    sink_input_idx = [None]  # mutable cell so the closures below can cache it

    def find_sink_input_index():
        try:
            out = subprocess.run(["pactl", "list", "sink-inputs"],
                                  capture_output=True, text=True, timeout=2).stdout
        except Exception:
            return None
        current_idx = None
        for line in out.splitlines():
            line_stripped = line.strip()
            if line_stripped.startswith("Sink Input #"):
                current_idx = line_stripped.split("#", 1)[1]
            elif "car_sim_radio_player" in line and current_idx is not None:
                return current_idx
        return None

    # REAL BUG FOUND AND FIXED (2026-09-22, Muni: "the volume slide
    # completely lags the program... i think all inputs are on the main
    # thread" -- exactly right): Tkinter's Scale fires its `command`
    # continuously while dragging, and each call used to run `pactl`
    # (a subprocess spawn + wait, tens of ms each) directly on the Tk
    # mainloop thread -- blocking the ENTIRE GUI (including the elapsed-time
    # refresh and Back/Next buttons) for the whole drag gesture. Fixed by
    # moving all the actual `pactl` work onto a dedicated background thread;
    # the Tk-thread callback below only ever does a non-blocking queue put,
    # never touches subprocess directly. A maxsize=1 queue that always
    # keeps just the LATEST requested value (dropping any older
    # still-pending one) naturally coalesces a fast drag into far fewer
    # actual pactl calls, without a fixed debounce timer.
    volume_request_q = queue.Queue(maxsize=1)

    def volume_worker():
        while True:
            percent = volume_request_q.get()
            while True:  # drain to the latest, in case more piled up already
                try:
                    percent = volume_request_q.get_nowait()
                except queue.Empty:
                    break
            idx = sink_input_idx[0]
            if idx is None:
                idx = find_sink_input_index()
                sink_input_idx[0] = idx
            if idx is None:
                root.after(0, lambda: volume_status_var.set("volume: player stream not found yet"))
                continue
            result = subprocess.run(
                ["pactl", "set-sink-input-volume", idx, f"{percent}%"],
                capture_output=True, text=True)
            if result.returncode != 0:
                # Stream likely disappeared (player restarted) -- drop the
                # cached index so the next slider move re-looks-it-up.
                sink_input_idx[0] = None
                root.after(0, lambda: volume_status_var.set("volume: player stream not found yet"))
            else:
                root.after(0, lambda p=percent: volume_status_var.set(f"volume: {p}% (this app only)"))

    threading.Thread(target=volume_worker, daemon=True).start()

    def set_app_volume(percent_str):
        percent = int(float(percent_str))
        try:
            volume_request_q.put_nowait(percent)
        except queue.Full:
            try:
                volume_request_q.get_nowait()
            except queue.Empty:
                pass
            try:
                volume_request_q.put_nowait(percent)
            except queue.Full:
                pass

    vol_frame = tk.Frame(root)
    vol_frame.pack(pady=(6, 0))
    tk.Label(vol_frame, text="Volume (this app only):").pack(side=tk.LEFT, padx=(0, 6))
    volume_status_var = tk.StringVar(value="volume: not yet changed")
    volume_scale = tk.Scale(vol_frame, from_=0, to=150, orient=tk.HORIZONTAL,
                             length=180, showvalue=False, command=set_app_volume)
    volume_scale.pack(side=tk.LEFT)
    tk.Label(root, textvariable=volume_status_var, font=("Sans", 8)).pack()

    # Sync the SLIDER'S DISPLAYED POSITION to whatever the volume actually
    # already is (2026-09-22, Muni: "volume should default to what it was
    # before, the slider"). This is a READ-ONLY query (`pactl list
    # sink-inputs`), never a write -- explicitly distinct from the earlier,
    # explicitly-rejected "auto-force volume to 100% on launch" feature
    # (see this file's own git history/STATUS.md for why that was reverted:
    # Muni: "DEFINITELY DO NOT ADD THAT... quit changing my volume"). The
    # slider was previously just defaulting to Tkinter's own from_=0 with no
    # sync at all, silently lying about the real current level. Retries a
    # few times since the player's PulseAudio stream doesn't exist
    # immediately at launch.
    def sync_volume_slider_display(attempts_left=20):
        idx = find_sink_input_index()
        if idx is None:
            if attempts_left > 0:
                root.after(300, lambda: sync_volume_slider_display(attempts_left - 1))
            return
        sink_input_idx[0] = idx
        try:
            out = subprocess.run(["pactl", "list", "sink-inputs"],
                                  capture_output=True, text=True, timeout=2).stdout
        except Exception:
            return
        in_target = False
        for line in out.splitlines():
            stripped = line.strip()
            if stripped.startswith("Sink Input #"):
                in_target = (stripped == f"Sink Input #{idx}")
            elif in_target and stripped.startswith("Volume:"):
                m = re.search(r"(\d+)%", stripped)
                if m:
                    percent = int(m.group(1))
                    volume_scale.config(command="")  # suppress the write-back this .set() would otherwise trigger
                    volume_scale.set(percent)
                    volume_scale.config(command=set_app_volume)
                    volume_status_var.set(f"volume: {percent}% (this app only)")
                break

    root.after(200, sync_volume_slider_display)

    audio_level_var = tk.StringVar(value="")
    tk.Label(root, textvariable=audio_level_var, font=("Sans", 9)).pack(pady=(4, 0))

    decode_error_var = tk.StringVar(value="")
    decode_error_label = tk.Label(root, textvariable=decode_error_var,
                                   font=("Sans", 9, "bold"), fg="red")
    decode_error_label.pack(pady=(2, 0))

    # How long a warning stays visible after the last actual decode error --
    # long enough to be readable/noticeable, short enough to clear
    # naturally once the stream has genuinely recovered (matches this
    # session's own observed error-burst duration around a live-serve jump).
    DECODE_ERROR_DISPLAY_SECS = 2.0

    def refresh():
        idx, n, total_read, lap_bytes, alive, paused = state.snapshot()
        now_playing_var.set(f"Now Playing: {state.files[idx]['name']}")
        elapsed_secs = lap_bytes // ASSUMED_BYTES_PER_SEC
        elapsed_var.set(f"{elapsed_secs // 60}:{elapsed_secs % 60:02d}")
        # "reconnecting" not "reader stopped" (2026-09-22): with
        # reconnect_supervisor() now auto-restarting playback once the
        # device reappears, `alive=False` is a transient in-progress state,
        # not a dead end the way it used to be before auto-resume existed.
        if not alive:
            suffix = "  [reconnecting...]"
        elif paused:
            suffix = "  [PAUSED]"
        else:
            suffix = ""
        status_var.set(f"file {idx + 1}/{n}  |  bytes read: {total_read}{suffix}")
        since_last_level = time.monotonic() - audio_level_state["last_ms"]
        if audio_level_state["rms_db"] is None or since_last_level > 1.0:
            # No reading yet, or stale (player not running/crashed, or
            # --player mpg123/mpg123-strict, which don't emit this at all --
            # this filter is only wired into the default ffmpeg player).
            audio_level_var.set("")
        elif audio_level_state["rms_db"] <= SILENCE_THRESHOLD_DB:
            audio_level_var.set("🔇 silence")
        else:
            audio_level_var.set(f"🔊 audio  |  level: {audio_level_state['rms_db']:.0f} dB")
        since_last_error = time.monotonic() - decode_error_state["last_ms"]
        if decode_error_state["count"] > 0 and since_last_error < DECODE_ERROR_DISPLAY_SECS:
            decode_error_var.set(f"⚠ MP3 DECODE ERROR (a real radio would glitch/mute here) "
                                  f"-- {decode_error_state['count']} total this session")
        else:
            decode_error_var.set("")
        root.after(200, refresh)

    def on_close():
        writer_stop.set()
        reconnect_stop.set()
        if player:
            try:
                player.stdin.close()
            except BrokenPipeError:
                pass
        if level_proc:
            try:
                level_proc.stdin.close()
            except (BrokenPipeError, OSError):
                pass
            level_proc.terminate()
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_close)
    root.after(200, refresh)
    print("[radio][gui] window open -- close it (or Ctrl-C here) to stop", file=sys.stderr)
    root.mainloop()

    if player:
        try:
            player.stdin.close()
        except BrokenPipeError:
            pass
        player.wait()
    if capture_f:
        capture_f.close()
    print(f"[radio][gui] done. total bytes read: {state.total_read}", file=sys.stderr)


ESPRESSIF_USB_VENDOR_ID = "303a"


def find_s3_block_device():
    """Auto-detect the real S3's /dev/sdX block device by USB vendor ID
    (2026-09-22, Muni: "cant u make the gui auto find the drive so we dont
    have this issue all the time"). Added after a real, confirmed bug: the
    S3's /dev/sdX path is NOT stable across its own reboots (every reflash
    re-enumerates it, often to a different letter) -- a long-running GUI
    process kept reading from a stale path after later reflashes, producing
    constant decode garbage that looked exactly like a firmware bug but
    wasn't one. Scans /dev/sd? (single letter only, so a partition like
    /dev/sda1 is never matched) and checks each one's real udev properties
    for Espressif's actual vendor ID (303a) -- not a name/label guess.
    """
    import glob
    candidates = sorted(glob.glob("/dev/sd?"))
    for dev in candidates:
        try:
            out = subprocess.run(["udevadm", "info", "-q", "property", "-n", dev],
                                  capture_output=True, text=True, timeout=2).stdout
        except Exception:
            continue
        for line in out.splitlines():
            if line.strip() == f"ID_VENDOR_ID={ESPRESSIF_USB_VENDOR_ID}":
                return dev
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9003, help="port to connect to (s3_sim_serial.py, or a real-hardware bridge)")
    ap.add_argument("--capture", help="also write the extracted MP3 stream to this file")
    ap.add_argument("--no-play", action="store_true", help="don't launch a player, just read/capture")
    ap.add_argument("--throttled-sink", action="store_true",
                     help="DIAGNOSTIC ONLY: instead of a real player, spawn a trivial "
                          "rate-limited discard sink (no audio decode, no real device) that "
                          "only reads at the real MP3 bitrate -- isolates whether real-time "
                          "PACING ALONE (independent of any actual audio decode/output "
                          "complexity) is enough to reproduce a suspected periodic stall.")
    ap.add_argument("--simulate-radio-resume-cache", action="store_true",
                     help="OFF by default -- this tool's whole point is being a dumb, direct "
                          "reader with no memory of its own (see the module docstring). "
                          "Opt-in simulation of a theorized REAL car radio behavior, not "
                          "confirmed fact: a real radio hung mid-file on first pairing during "
                          "actual hardware testing (2026-09-17), and one plausible explanation "
                          "was the radio caching 'resume playback position' keyed on volume "
                          "serial + filename -- if the S3's ring content changed underneath an "
                          "identical-looking volume, resuming into a stale position would "
                          "explain exactly that symptom. When enabled, persists the last read "
                          "cluster position per (volume serial, filename) to "
                          "sim/.car_sim_resume_cache.json and resumes from there on a matching "
                          "reconnect, instead of always starting at cluster 0 -- lets the real "
                          "firmware's random-per-boot volume serial (fixed the same night, see "
                          "progress/STATUS.md) be validated against this specific theorized "
                          "failure mode entirely on a PC, without needing another physical "
                          "car-radio trip to find out if it actually defeats it.")
    ap.add_argument("--gui", action="store_true",
                     help="Launch a GUI window emulating what the car radio's own screen + "
                          "physical next/prev buttons would do: discovers ALL files present "
                          "(not just the first) and lets you switch between them like a real "
                          "radio's own file navigation would, to test PLAN_NEXT.md's C3 "
                          "(server-detected radio-button presses relayed to the classic ESP32) "
                          "end-to-end against real hardware. Requires the S3 stand-in "
                          "(sim/s3_real_firmware_host_multi, built with -DFATDISK_MULTI_FILE) "
                          "to actually be presenting multiple files -- against the normal "
                          "single-file build this just shows one file with disabled buttons. "
                          "Not yet wired up to --simulate-radio-resume-cache or "
                          "--throttled-sink.")
    ap.add_argument("--player", choices=["ffmpeg", "mpg123", "mpg123-strict"], default="ffmpeg",
                     help="--gui only: which player decodes the stream. ffmpeg (default) is "
                          "tuned for minimum latency but cannot cleanly resync after a real "
                          "content discontinuity in this ring (a known, already-confirmed "
                          "limitation -- see real_s3_listen.py). mpg123 is built for real-world "
                          "streaming (internet radio) and resyncs past discontinuities cleanly -- "
                          "use this when testing against a FATDISK_ALWAYS_SERVE_LIVE S3 build, "
                          "which deliberately introduces a jump whenever its cursor catches up "
                          "to live. mpg123-strict (added 2026-09-21, after a real car test failed "
                          "despite THIS tool's own mpg123 test passing) uses mpg123's own small "
                          "DEFAULT resync window (~1024 bytes) instead of --resync-limit -1's "
                          "unlimited tolerance -- a real, cheap embedded car-radio decoder almost "
                          "certainly does NOT have mpg123's unlimited-search resync behavior, so "
                          "a design that only 'works' under --resync-limit -1 was never actually "
                          "validated against anything resembling the real target hardware's own "
                          "tolerance. If FATDISK_ALWAYS_SERVE_LIVE can't survive mpg123-strict "
                          "cleanly, it likely can't survive the real radio's decoder either.")
    ap.add_argument("--skip-scan", action="store_true",
                     help="--gui only: skip the mount-time validity scan (scan_file_validity()) "
                          "that now runs by default before playback starts. The scan reads the "
                          "ENTIRE declared file once up front and reports raw zero-fill runs / "
                          "MP3 frame-sync density -- added 2026-09-21 after this exact gap let a "
                          "real firmware bug (a boot-primer tail-gap) through undetected on a "
                          "prior bench test. Only skip this for a quick, repeat test where the "
                          "file's already been validated once this session.")
    ap.add_argument("--bursty", action="store_true",
                     help="--gui only: let reads race up to BURST_AHEAD_SECONDS ahead of the "
                          "real-time playback schedule (pausing only once that bound would be "
                          "exceeded) instead of the default smooth, continuously-paced-to-exactly-"
                          "16000B/s pattern -- delivery to the player is ALWAYS continuous either "
                          "way, only the read-request timing bursts ahead. Added 2026-09-21 after "
                          "a real car test failed despite this tool passing under smooth pacing: "
                          "the smooth pattern is structurally incapable of ever reading FASTER than "
                          "the real encode rate, so it could never exercise a real firmware bug "
                          "(the live-serve cursor's ahead-of-writer drift correction) that only "
                          "fires when the reader genuinely races ahead -- which a real embedded "
                          "decoder doing internal read-ahead buffering very plausibly does. Two "
                          "earlier batch-based versions of this flag had their own real, confirmed "
                          "test-tool-only bugs (player stalls, then compounding drift from reading "
                          "in silent batches) -- see this flag's own git history / gui_read_loop's "
                          "comment for the full story before trusting a still-imperfect result.")
    ap.add_argument("--device",
                     help="Read from a REAL block device instead of the fake TCP protocol -- "
                          "e.g. /dev/sda once the REAL physical S3 board's own USB-OTG port is "
                          "plugged into this machine and enumerates as a real USB drive. This is "
                          "genuinely the same access a real car radio's own USB-MSC host driver "
                          "would have; --port/the TCP stand-in is not used at all when this is "
                          "given. Access is fully automated via HelperDeviceTransport (2026-09-21) "
                          "-- a small, separate, short-lived root-only helper opens just the "
                          "device fd and hands it back over a Unix socket, so THIS process (and "
                          "the audio player it spawns) keeps running as your normal user the "
                          "whole time -- no interactive password prompt, and real audio output "
                          "works correctly (running the whole process as root broke it -- see "
                          "HelperDeviceTransport's own docstring). Pass the literal value 'auto' "
                          "(2026-09-22) to auto-detect the real S3's block device by USB vendor ID "
                          "instead of guessing/remembering a path -- added specifically because "
                          "the S3's /dev/sdX path is NOT stable across its own reboots (every "
                          "reflash re-enumerates it, often to a different letter), which caused a "
                          "real, confusing bug tonight: a long-running GUI process kept reading "
                          "from a now-stale/nonexistent path after later reflashes, producing "
                          "constant decode garbage that looked exactly like a firmware bug but "
                          "wasn't one.")
    args = ap.parse_args()
    # Recorded BEFORE the "auto" resolution below overwrites args.device
    # with whatever real path it found -- run_gui_mode's reconnect_supervisor
    # needs to know whether to re-run find_s3_block_device() on every
    # reconnect attempt (a re-enumerated device can land on a different
    # /dev/sdX path, confirmed live 2026-09-22: sda -> sdb across one
    # unplug/replug cycle) or just recheck the one fixed path the user gave.
    args.device_was_auto = (args.device == "auto")

    if args.device == "auto":
        args.device = find_s3_block_device()
        if args.device is None:
            print("[radio] --device auto: no USB-MSC device with Espressif's vendor ID "
                  "(303a) found. Is the S3 plugged in and running?", file=sys.stderr)
            return
        print(f"[radio] --device auto: found S3 at {args.device}", file=sys.stderr)

    if args.gui and (args.simulate_radio_resume_cache or args.throttled_sink):
        print("[radio] --gui doesn't support --simulate-radio-resume-cache or "
              "--throttled-sink yet -- drop one of these", file=sys.stderr)
        return

    sock = None
    if args.device:
        # REAL BUG FOUND AND FIXED (2026-09-21): plain DeviceTransport needs
        # root (the real S3 device node is root:disk-owned, and this
        # account isn't in the disk group) -- but running this WHOLE
        # process as root (e.g. via `sudo car_sim.py`) breaks real
        # PulseAudio/PipeWire output for the mpg123/ffmpeg player subprocess
        # (confirmed live: a root-owned audio client's stream gets silently
        # dropped shortly after connecting). HelperDeviceTransport avoids
        # this entirely -- only a tiny, separate, short-lived helper
        # process touches root (just to open the device fd, handed back via
        # Unix-socket fd-passing), while THIS process (and therefore the
        # player it spawns) stays running as the normal user throughout.
        print(f"[radio] opening real block device {args.device} "
              f"(via root-helper, no interactive prompt)...", file=sys.stderr)
        transport = HelperDeviceTransport(args.device, SUDO_PASSWORD)
    else:
        print(f"[radio] connecting to board on port {args.port}...", file=sys.stderr)
        sock = socket.create_connection(("127.0.0.1", args.port))
        # A stuck request has no reason to hang forever, and an unbounded
        # sock.recv() has no way to fail loudly if the server-side does.
        # Confirmed directly: this hung silently for the rest of a real
        # session once, with no exception, no log line, no visible cause.
        sock.settimeout(15.0)
        transport = TcpTransport(sock)
    print("[radio] connected, reading boot sector...", file=sys.stderr)

    player = None
    capture_f = None
    total_read = 0
    try:
        boot = transport.read(0, 1)
        layout = parse_boot_sector(boot)
        print(f"[radio] parsed BPB: {layout}", file=sys.stderr)

        fat = transport.read(layout["reserved_sectors"], layout["fat_size_sectors"])

        root_dir = transport.read(layout["root_dir_lba"], layout["root_dir_sectors"])

        if args.gui:
            run_gui_mode(transport, layout, fat, root_dir, args)
            return

        entry = find_file_entry(root_dir)
        if not entry:
            print("[radio] no file found in root directory, aborting", file=sys.stderr)
            return
        print(f"[radio] found file: {entry}", file=sys.stderr)

        cluster_list = []
        c = entry["first_cluster"]
        eoc = end_of_chain_marker(layout["fat_type"])
        # Hard safety bound (65524 is FAT16's own max valid cluster count,
        # the largest either variant here can legitimately produce) --
        # guards against ANY future variant of the bug above (a
        # misdetected/corrupt FAT type producing a chain that never hits
        # its end marker) hanging this process forever / exhausting
        # memory again, instead of just trusting the loop to terminate.
        while c < eoc and len(cluster_list) <= 65524:
            cluster_list.append(c)
            c = fat_entry(fat, c, layout["fat_type"])
        num_clusters = len(cluster_list)

        start_cluster_pos = 0
        if args.simulate_radio_resume_cache:
            cache = load_resume_cache()
            key = resume_cache_key(layout["volume_serial"], entry["name"])
            cached_pos = cache.get(key)
            if cached_pos is not None and cached_pos < num_clusters:
                start_cluster_pos = cached_pos
                print(f"[radio] simulated resume cache hit for {key} -- "
                      f"resuming at cluster_pos={start_cluster_pos} instead of 0 "
                      f"(this is the theorized real-radio behavior, not this tool's "
                      f"normal default)", file=sys.stderr)
            else:
                print(f"[radio] simulated resume cache: no entry for {key}, starting at 0",
                      file=sys.stderr)

        if args.throttled_sink:
            # Reads bytes as fast as the OS delivers them but only ever
            # discards them at the real MP3 bitrate, matching the pacing a
            # real-time audio player enforces -- but doing ZERO actual
            # decode/device work. If this ALONE reproduces the stall,
            # real-time pacing itself (not audio decode/device complexity)
            # is implicated. If it doesn't, the stall genuinely needs a
            # real player/audio-device relationship to appear.
            player = subprocess.Popen(
                ["python3", "-u", "-c",
                 "import sys, time\n"
                 "rate = 128000/8\n"
                 "next_tick = time.monotonic()\n"
                 "while True:\n"
                 "    chunk = sys.stdin.buffer.read(4096)\n"
                 "    if not chunk:\n"
                 "        break\n"
                 "    next_tick += len(chunk) / rate\n"
                 "    sleep_time = next_tick - time.monotonic()\n"
                 "    if sleep_time > 0:\n"
                 "        time.sleep(sleep_time)\n"],
                stdin=subprocess.PIPE,
            )
        elif not args.no_play:
            # ROOT CAUSE FOUND AND FIXED (see the explicit .flush() call
            # below, not anything in this Popen call). Direct per-thread
            # /proc/PID/task/TID/wchan polling at 20Hz showed ffmpeg's own
            # DEMUXER thread (`dmx0:mp3`, doing read() on this process's
            # stdin) cycling ~1s of anon_pipe_read then ~7.5s of
            # futex_do_wait, endlessly repeating -- while decode/output sat
            # blocked on the same futex waiting for packets, proving they
            # had nothing to do because the demuxer had stopped pulling
            # from stdin. `-thread_queue_size` (below) was an earlier,
            # WRONG fix attempt for this -- directly measured on an
            # isolated repro to have zero effect (tested at both the
            # default 8 and 4096, identical stall either way). A separate
            # isolated repro (reusing the real GrowingFat12Disk/serve_radio
            # retry logic with a synthetic real-time writer, no ESP32/BT
            # involved at all) reproduced the exact same cycle, which ruled
            # out ffmpeg/PipeWire internals and pointed back at THIS
            # process's own write loop. The actual cause: player.stdin is a
            # subprocess.Popen default bufsize=-1 pipe, which Python wraps
            # in an io.BufferedWriter -- data handed to .write() sits in a
            # user-space buffer and isn't necessarily pushed into the
            # actual OS pipe ffmpeg reads from. Confirmed directly: adding
            # an explicit .flush() after every write (below) eliminated the
            # stall completely on the isolated repro across two independent
            # 90s/120s runs (zero futex_do_wait transitions, vs. a reliable
            # ~8.5s cycle without it).
            player = subprocess.Popen(
                ["chrt", "--rr", "50", "ffmpeg", "-loglevel", "warning",
                 "-thread_queue_size", "4096",
                 "-fflags", "nobuffer", "-flags", "low_delay",
                 "-analyzeduration", "0", "-probesize", "4096", "-f", "mp3",
                 "-i", "-", "-f", "pulse",
                 # Every ffmpeg instance shares the same default PipeWire/Pulse
                 # client identity ("Lavf<version>"), and module-stream-restore
                 # persists mute/routing state keyed on that identity -- muting
                 # any diagnostic ffmpeg instance (of which there were many
                 # during development) silently persisted and got reapplied to
                 # THIS player on its next restart, twice leaving the real
                 # output muted with no error. -name gives this player its own
                 # identity so its saved state can never be touched by another
                 # ffmpeg process's.
                 "-name", "car_sim_radio_player",
                 "-prebuf", "0", "-buffer_duration", "50", "default"],
                stdin=subprocess.PIPE,
            )
        capture_f = open(args.capture, "wb") if args.capture else None

        print("[radio] playing (Ctrl-C to stop)...", file=sys.stderr)
        cluster_pos = start_cluster_pos
        last_cache_save = time.monotonic() if args.simulate_radio_resume_cache else None
        while True:
            cluster = cluster_list[cluster_pos]
            lba = layout["data_lba"] + (cluster - 2) * layout["sectors_per_cluster"]
            data = transport.read(lba, layout["sectors_per_cluster"])
            if capture_f:
                capture_f.write(data)
            if player:
                # The only backpressure in this whole program: this call
                # blocks once mpg123's stdin pipe is full, exactly like a
                # real decoder's input FIFO filling up would. Nothing
                # here paces itself against any assumed bitrate.
                try:
                    player.stdin.write(data)
                    # Without this, data can sit in Python's own
                    # BufferedWriter instead of reaching ffmpeg's stdin
                    # pipe -- see the root-cause comment above.
                    player.stdin.flush()
                except BrokenPipeError:
                    print("[radio] player exited, stopping", file=sys.stderr)
                    break
            total_read += len(data)
            cluster_pos = (cluster_pos + 1) % num_clusters
            if args.simulate_radio_resume_cache and time.monotonic() - last_cache_save >= 1.0:
                save_resume_position(layout["volume_serial"], entry["name"], cluster_pos)
                last_cache_save = time.monotonic()
    except KeyboardInterrupt:
        pass
    except FileNotFoundError:
        print("[radio] mpg123 not found -- install it or pass --no-play", file=sys.stderr)
    except OSError as e:
        print(f"[radio] socket error, giving up: {e}", file=sys.stderr)

    if player:
        try:
            player.stdin.close()
        except BrokenPipeError:
            pass
        player.wait()
    if capture_f:
        capture_f.close()
    print(f"[radio] done. total bytes read: {total_read}", file=sys.stderr)


if __name__ == "__main__":
    main()
