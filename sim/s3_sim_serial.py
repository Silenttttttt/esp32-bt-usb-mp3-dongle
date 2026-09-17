#!/usr/bin/env python3
"""Synthetic stand-in for the real S3, now talking to the REAL ESP32 over
its USB-serial link (standing in for the eventual wired UART link -- this
is the option that actually works: WiFi was tried first and confirmed, by
direct heap measurement, to starve Bluetooth's own init on this chip).
Same disk-serving role as s3_sim.py / s3_sim_wifi.py: receives the live
MP3 stream, appends it into a real growing FAT12 volume, and serves it to
the radio via SCSI READ10-style sector reads.

Parses the ESP32's framed protocol (1-byte type + 4-byte big-endian length
+ payload) so real AVRCP play/pause/track-change events from a real phone
show up as real, timestamped log lines.
"""
import argparse
import gc
import glob
import serial
import socket
import struct
import subprocess
import sys
import threading
import time

from fat12_disk import GrowingFat12Disk
from sector_protocol import recv_read10_request, OPCODE_READ10

# Testing a specific hypothesis: three back-to-back reader-side tests
# (mpg123, ffmpeg, ffmpeg+car_sim.py's own gc.disable()) all showed the
# IDENTICAL ~8.5s stall periodicity despite the reader process being
# freshly restarted (new PID, new interpreter, new GC state) each time --
# ruling out anything in car_sim.py's own process as the cause. The one
# thing that stayed constant across all three was THIS process (unchanged
# PID throughout). Disabling GC here too, as the same cheap, directly
# falsifiable test applied to the one process that's actually been
# constant.
gc.disable()

disk_lock = threading.Lock()


FRAME_MAGIC = 0xAA
# Generous vs. real audio/control chunks (a BT-audio frame is a few hundred
# bytes) but bounded well under the ring capacity -- a corrupted-but-in-range
# length used to be able to reach append()'s "bigger than the whole ring"
# branch and wipe the ring's entire history in one call. Must stay below
# whatever --capacity-mb is in use (default ring is 480,038 bytes / ~0.46MB);
# 4096 is still ~8-40x any real frame with plenty of room to spare.
MAX_FRAME_LEN = 4_096

# How long a read is allowed to wait for a straddle (see
# fat12_disk.read_sectors' avoid_straddle docstring) to clear before giving
# up and serving a torn read anyway. In theory write_pos should only spend
# about one cluster-duration (~0.26s at 128kbps) inside a given cluster's
# byte range -- but measured directly, a 100-retry/500ms budget still let
# ~2% of reads (30 of 1506 in one real test run) exhaust retries and get
# served torn anyway, which is exactly the residual "jagged/clippy" audio
# reported on top of the mostly-fixed skip-back. 150 retries bounds the
# worst case at 750ms. MP3 decode-side buffering already puts the real
# end-to-end latency floor at ~9.5-10.6s (confirmed directly, independent
# of anything in this pipeline) and the user's own stated tolerance is
# 5-10s given otherwise-clean audio, so there's real room here -- no need
# to be stingy the way an earlier, wrong "keep it under 2s" assumption was.
READ_RETRY_DELAY = 0.005
# CONFIRMED REGRESSION, found by adversarial verification, fixed here: this
# constant was tuned against the OLD READ_SAFETY_MARGIN (2 clusters), whose
# worst-case cold-start clear time is (2*4096+4096)/16000 = 0.77s -- 300
# retries (1.5s) gave that case ~2x headroom. When the margin was widened to
# 16 clusters (see below) to fix the torn-read/margin-erosion bug, nobody
# re-checked this dependent budget: the new worst-case cold-start clear time
# is (16*4096+4096)/16000 = 4.352s, which EXCEEDS the old 1.5s budget by ~3x.
# Direct simulation against the real read_sectors() (sweeping write_pos
# across the full ring at connect time) confirmed this was a real, live bug:
# ~22.5% of possible connect phases would exhaust the old budget and serve
# a torn read (up to 3 consecutive) purely from bad luck on the very first
# few reads -- every isolated-repro test this session happened to use a
# fixed `sleep 1` before connecting, which consistently landed in a safe
# phase and never exercised the other ~77.5%. Raised to 1400 retries (7.0s)
# for ~1.6x real headroom over the new 4.352s worst case. This is a
# ONE-TIME cost on the very first few reads after a (re)connect only --
# not a recurring steady-state cost -- and stacks on top of the already-
# accepted ~9.5-10.6s MP3-decoder buffering floor, which is the dominant
# term in total time-to-first-sound regardless.
MAX_READ_RETRIES = 1400

# Real breathing room between the reader's requested cluster and the
# writer's live edge -- see fat12_disk.read_sectors' avoid_straddle
# docstring for the full story. Without this (margin=0), direct
# measurement showed ~90%+ of reads needing 40-58 retries as a STEADY
# STATE (not an edge case), because with zero margin the reader ends up
# parked exactly at the writer's live edge for the pipeline's entire
# lifetime -- any small timing jitter on top of that baseline was enough
# to push several consecutive reads to their retry ceiling, producing
# real ~3.5s hard-mute periods roughly every ~11s.
#
# 2 cluster-widths (the original value here) turned out to be nowhere
# near enough once car_sim.py stopped needing frequent restarts (after
# the player.stdin.flush() fix below): direct measurement on the real
# live pipeline showed the reader running ~10% faster than the real
# writer (BT audio ingestion, itself running below its nominal 128kbps
# due to the known PCM_DROPS/Shine-CPU-budget issue on the ESP32) --
# with only a 2-cluster margin, that gap eroded within minutes and then
# EVERY straddle exhausted the full retry budget and got served torn
# (a real audible glitch roughly every 4-5s). Raised to 16 cluster-widths
# after validating on an isolated, hardware-free repro (reusing this same
# retry logic against a synthetic writer deliberately slowed to match the
# real measured ~15,200 B/s deficit): torn_served stayed at exactly 0 over
# 600+ continuous seconds at 16 clusters, including against a BURSTY drop
# pattern (6 consecutive dropped frames every ~2.95s, mimicking a real
# CPU-stall spike rather than a smooth shortfall) -- only 2 torn reads
# during the very first few seconds after connect (before any margin has
# been established at all, unavoidable at any margin value), then zero
# for the rest of the run. Smaller values (4 and 8 clusters) also proved
# stable against a SMOOTH deficit model but were not validated against
# the bursty model, so 16 was chosen as the value with direct evidence
# against the harder, more realistic failure pattern. Costs ~4.1s of
# additional ONE-TIME first-connect latency (not a per-read recurring
# cost) -- accepted given the user's stated tolerance and the much larger
# existing ~9.5-10.6s MP3-decoder buffering floor this stacks on top of.
READ_SAFETY_MARGIN = 16 * 4096  # 16 clusters (4096B each: SECTORS_PER_CLUSTER=8 * SECTOR_SIZE=512)

# Mirror of READ_RETRY_DELAY/MAX_READ_RETRIES, but for the WRITE side's new
# unread_protect check (see fat12_disk.GrowingFat12Disk.append()'s
# docstring for the real gap this closes: nothing previously stopped the
# writer from overwriting content the reader hadn't reached yet if the
# READER side ever genuinely stalled). In normal operation this should
# essentially never actually wait -- the reader and writer both progress
# at the same real-time rate by design (a real car radio reads at exactly
# the encoded bitrate, and the silence-injection fix on the ESP32 keeps
# the writer producing at that same rate through any pause/disconnect) --
# so a short budget is enough to absorb ordinary jitter without risking a
# long stall on receive_from_esp32() itself (which is reading live bytes
# off the actual serial port; blocking it too long risks backing up the
# ESP32's own encode/serial-write pipeline). 200 retries at 5ms = 1.0s --
# if the reader is still stalled after a full second, something else is
# genuinely wrong and forcing the write through (never silently dropping
# real audio) is the right last resort, same philosophy as the read side's
# own bounded retry-then-force fallback.
WRITE_RETRY_DELAY = 0.005
MAX_WRITE_RETRIES = 200

# REAL, RECURRING FAILURE MODE FOUND LIVE (Muni actually driving/testing
# tonight, no way to physically intervene): when the ESP32 crashes and
# reboots (a residual, not-fully-eliminated reconnect-crash bug -- see
# STATUS.md), its USB-CDC device can re-enumerate to a DIFFERENT
# /dev/ttyACMx path, silently orphaning this script's already-open file
# handle. Confirmed three times tonight: the old handle does NOT reliably
# raise an I/O error when this happens (a warm, software-triggered reboot
# doesn't always look like a hard physical unplug to the kernel) -- it can
# just keep returning empty reads forever, meaning receive_from_esp32()
# silently stops making progress with NO error, NO log line, nothing to
# indicate anything's wrong, while car_sim.py/serve_radio() (driven
# independently by the ring's own contents, not by whether the ESP32 side
# is alive) keeps running normally, masking the failure completely. Fixing
# this required manual intervention every time tonight (finding the new
# path by serial number, fixing a stale USB permission, restarting this
# script) -- completely unacceptable for a system meant to run unattended
# in a moving car. The fix below makes the whole serial connection
# self-healing with zero human intervention: resolve the port by SERIAL
# NUMBER (not a fixed path, which can change), detect staleness directly
# (don't rely on an I/O error that may never come), and automatically
# close/re-resolve/re-open forever whenever anything goes wrong.
EXPECTED_SERIAL = "5B52096812"  # the correct project board -- NEVER 5B07008126 (Fin-ESP, unrelated)
STALE_TIMEOUT = 5.0  # seconds of zero bytes received => assume the connection is dead
PORT_SETTLE_RETRIES = 10
PORT_SETTLE_DELAY = 0.5


class SerialStaleError(Exception):
    """Raised when no bytes have arrived for longer than STALE_TIMEOUT --
    see the real incident documented above EXPECTED_SERIAL."""


def resolve_serial_port(expected_serial):
    """Find the CURRENT /dev/ttyACMx path for the board with the given
    serial number, by SERIAL NUMBER rather than a fixed path -- a path
    can change on any USB re-enumeration (confirmed real, recurring
    tonight), but the serial number never does. Returns None if not
    currently found (e.g. genuinely unplugged)."""
    for path in sorted(glob.glob("/dev/ttyACM*")):
        try:
            result = subprocess.run(
                ["udevadm", "info", "-q", "property", "-n", path],
                capture_output=True, text=True, timeout=2)
        except Exception:
            continue
        for line in result.stdout.splitlines():
            if line.strip() == f"ID_SERIAL_SHORT={expected_serial}":
                return path
    return None


def open_serial_resilient(expected_serial, baud):
    """Resolve the port by serial number and open it, tolerating a real,
    confirmed race: a freshly re-enumerated /dev/ttyACMx node can briefly
    be unopenable (PermissionError) even when the running user's own
    group membership should normally already suffice -- most likely a
    timing race between the device node appearing and udev/logind's own
    permission-granting rules finishing. Retries with short delays rather
    than failing immediately or requiring a one-off manual `setfacl` fix
    (which is what this session did, repeatedly, before this rewrite).
    Blocks (retrying indefinitely, printing status) until the board is
    found and openable -- there's no sensible way to "give up" here, since
    giving up would mean permanently losing audio until a human
    physically intervenes, which is exactly what must never happen."""
    attempt = 0
    while True:
        attempt += 1
        path = resolve_serial_port(expected_serial)
        if path is None:
            if attempt == 1 or attempt % 10 == 0:
                print(f"[s3] board (serial {expected_serial}) not currently found on "
                      f"any /dev/ttyACM* -- waiting...", file=sys.stderr)
            time.sleep(PORT_SETTLE_DELAY)
            continue
        try:
            ser = serial.Serial()
            ser.port = path
            ser.baudrate = baud
            # Short per-call timeout so read_exact()'s staleness check
            # (below) can actually run periodically instead of blocking
            # for a long fixed duration on a dead connection.
            ser.timeout = 2.0
            # Opening a pyserial port normally toggles DTR/RTS, which on
            # this board is wired to the reset/EN pin (the same mechanism
            # arduino-cli's own upload step uses for "Hard resetting via
            # RTS pin") -- pre-setting both false avoids an unnecessary
            # reset-triggering transition on every (re)open.
            ser.dtr = False
            ser.rts = False
            ser.open()
            print(f"[s3] opened {path} (serial {expected_serial}) at {baud} baud "
                  f"(attempt {attempt})", file=sys.stderr)
            return ser
        except (serial.SerialException, PermissionError, OSError) as e:
            if attempt <= PORT_SETTLE_RETRIES or attempt % 10 == 0:
                print(f"[s3] open {path} failed (attempt {attempt}): {e!r} -- retrying...",
                      file=sys.stderr)
            time.sleep(PORT_SETTLE_DELAY)


def read_exact(ser, n):
    # A serial port going quiet isn't "closed" the way a socket is -- keep
    # waiting, but only for up to STALE_TIMEOUT seconds of TOTAL silence
    # (not per read_exact() call -- per byte of actual progress). A short
    # per-call ser.timeout just lets us poll for that; only genuine,
    # sustained silence (not routine gaps between BT audio frames) raises
    # SerialStaleError, which the caller (run_serial_bridge, below) uses
    # to trigger a full reconnect.
    data = b""
    last_progress = time.monotonic()
    while len(data) < n:
        chunk = ser.read(n - len(data))
        if chunk:
            data += chunk
            last_progress = time.monotonic()
        elif time.monotonic() - last_progress > STALE_TIMEOUT:
            raise SerialStaleError(
                f"no bytes received in {STALE_TIMEOUT}s -- assuming the serial "
                f"connection is dead")
    return data


def find_sync(ser):
    junk = bytearray()
    while True:
        b = read_exact(ser, 1)
        if b[0] == FRAME_MAGIC:
            if junk:
                print(f"[s3] discarded {len(junk)} non-sync bytes: {junk!r}", file=sys.stderr)
            return
        junk += b
        if len(junk) >= 200:
            print(f"[s3] discarded 200 non-sync bytes: {bytes(junk)!r}", file=sys.stderr)
            junk.clear()


def receive_from_esp32(ser, disk):
    total = 0
    control_count = 0
    start = time.monotonic()
    last_log = start
    # Direct, per-frame check for a gap in actual BT/serial reception itself
    # -- everything downstream (disk-serving retries, GC in both Python
    # processes, the source injection, the raw captured MP3 content, and
    # even the output audio device) has been individually ruled out as the
    # cause of a real, reproducible ~2s stall recurring every ~8.5s. This
    # is the one layer not yet directly instrumented at full resolution:
    # if Bluedroid/the ESP32 itself has a periodic reception gap, nothing
    # downstream would show it as anything other than "normal processing
    # of whatever data eventually arrives" -- this catches it at the
    # earliest possible point, one frame at a time, not averaged per-second.
    last_frame_time = start
    try:
        while True:
            find_sync(ser)
            header = read_exact(ser, 5)
            frame_type = chr(header[0])
            length = struct.unpack(">I", header[1:5])[0]

            # A dropped/corrupted/interleaved byte anywhere upstream (e.g. two
            # frames spliced together by concurrent writers on the ESP32 side)
            # can make an ordinary data byte look like a magic marker. Validate
            # both the type and the length before trusting them -- on mismatch,
            # this wasn't a real frame start, so rescan instead of blocking
            # forever in read_exact() on a phantom-sized payload.
            if frame_type not in ("A", "C") or length > MAX_FRAME_LEN:
                print(f"[s3] resync: bad header (type={header[0]!r} length={length}), rescanning",
                      file=sys.stderr)
                continue

            payload = read_exact(ser, length) if length else b""

            frame_now = time.monotonic()
            if frame_type == "A":
                frame_gap = frame_now - last_frame_time
                if frame_gap > 0.3:
                    print(f"[s3][FRAME_GAP] type=A gap={frame_gap*1000:.1f}ms "
                          f"payload_len={len(payload)} t={frame_now - start:.3f}s",
                          file=sys.stderr)
            last_frame_time = frame_now

            if frame_type == "A":
                for _ in range(MAX_WRITE_RETRIES):
                    with disk_lock:
                        if disk.append(payload, unread_protect=True):
                            break
                    time.sleep(WRITE_RETRY_DELAY)
                else:
                    # Reader genuinely hasn't moved in ~1s -- force the
                    # write through rather than stall serial ingestion
                    # indefinitely (same bounded-retry-then-force
                    # philosophy as the read side's own fallback).
                    with disk_lock:
                        disk.append(payload)
                total += len(payload)
            elif frame_type == "C":
                control_count += 1
                now = time.monotonic()
                text = payload.decode("utf-8", errors="replace")
                esp_ms, _, event = text.partition("|")
                print(f"[s3][CONTROL] t={now - start:7.3f}s  esp32_ms={esp_ms:>10}  event={event}",
                      file=sys.stderr)

            now = time.monotonic()
            if now - last_log >= 1.0:
                print(f"[s3][serial-rx][timing] t={now - start:6.2f}s  audio_received={total:8d}B  "
                      f"control_events={control_count}", file=sys.stderr)
                last_log = now
    except Exception as e:
        # Unguarded, this used to die silently on any serial-level error
        # (USB unplug, driver fault, or -- confirmed the real, common case
        # live tonight -- an ESP32 reboot re-enumerating to a new device
        # path) -- serve_radio() would keep running and keep serving the
        # now-permanently-frozen ring with zero indication ingestion had
        # stopped, indistinguishable from a real Bluetooth idle period.
        # Loud and unambiguous beats that. run_serial_bridge() (the
        # caller) catches this and reconnects automatically -- this is no
        # longer fatal to the whole process, just to this one connection
        # attempt.
        print(f"[s3][RECONNECTING] serial ingestion stopped: {e!r} -- reconnecting "
              f"automatically, no restart needed", file=sys.stderr)
        raise


def run_serial_bridge(expected_serial, baud, disk):
    """Supervisor around receive_from_esp32(): resolves the port by
    serial number, opens it (tolerating the permission-settle race),
    runs the real receive loop, and on ANY failure (a real I/O error, or
    SerialStaleError from prolonged silence) closes the handle and starts
    over -- forever. This is the actual fix for tonight's real, repeated
    need for manual intervention (finding a new /dev/ttyACMx path,
    fixing a stale permission, restarting the whole script by hand) every
    time the ESP32 crashed and reconnected -- completely unacceptable for
    unattended use in a moving car. Runs in its own thread; the TCP/
    disk-serving side (serve_radio(), driven by car_sim.py's connection)
    is completely unaffected by any of this and needs no changes at all."""
    while True:
        ser = open_serial_resilient(expected_serial, baud)
        try:
            receive_from_esp32(ser, disk)
        except Exception:
            pass  # already logged inside receive_from_esp32(); just reconnect
        finally:
            try:
                ser.close()
            except Exception:
                pass


def serve_radio(conn, disk):
    read_count = 0
    read_time_total = 0.0
    straddle_count = 0  # reads that needed at least one retry
    torn_count = 0  # retries exhausted, served anyway (should be ~never)
    start = time.monotonic()
    last_log = start
    try:
        while True:
            opcode, lba, count = recv_read10_request(conn)
            if opcode != OPCODE_READ10:
                break
            t0 = time.monotonic()
            data = None
            attempts = 0
            # Real bug, found via direct testing (pausing the live source):
            # avoid_straddle only knows "is write_pos inside the unsafe
            # zone right now" -- it has no notion of "the writer has
            # genuinely stopped." When a real source pauses (a real phone
            # pausing music is the exact real-world equivalent), write_pos
            # parks permanently inside whatever cluster it last touched,
            # and every subsequent read of that cluster would exhaust the
            # full retry budget and get marked torn FOREVER, even though
            # nothing is actually concurrently writing -- there's no
            # tearing risk at all once the writer is confirmed idle.
            # Tracking write_pos across the retry loop and bailing out
            # early (as a genuine safe read, not a torn one) the moment it
            # demonstrably hasn't moved fixes this without weakening the
            # real protection for the case that matters (an ACTIVELY
            # writing source).
            initial_write_pos = disk.write_pos
            # Only start treating "write_pos hasn't moved" as evidence of a
            # genuinely idle writer after giving an ACTIVE writer a fair,
            # realistic chance to advance first -- checking from attempt 1
            # would trivially always be true (no time has passed for a
            # real writer to move yet either) and would defeat the whole
            # straddle protection for the exact case it exists to protect.
            # ~150ms (30 retries) is comfortably longer than the ~0.26s
            # theoretical single-cluster write time only in the sense that
            # it's a meaningful fraction of it -- long enough that a truly
            # active writer will have moved at least somewhat, short
            # enough to still resolve a real pause quickly.
            idle_check_after = 30
            for _ in range(MAX_READ_RETRIES):
                attempts += 1
                with disk_lock:
                    data = disk.read_sectors(lba, count, margin=READ_SAFETY_MARGIN)
                    if (data is None and attempts >= idle_check_after
                            and disk.write_pos == initial_write_pos):
                        # Writer hasn't moved in ~150ms -- genuinely idle,
                        # not concurrently tearing anything.
                        data = disk.read_sectors(lba, count, avoid_straddle=False)
                if data is not None:
                    break
                time.sleep(READ_RETRY_DELAY)
            else:
                # Real hardware can't stall a USB host forever either --
                # after the bounded wait above, serve whatever's there now
                # rather than hang. Should be rare in practice (see the
                # budget comment above); a torn read here beats a timed-out
                # READ10 that could get the whole device reset by the host.
                with disk_lock:
                    data = disk.read_sectors(lba, count, avoid_straddle=False)
                torn_count += 1
            elapsed = time.monotonic() - t0
            if attempts > 1:
                straddle_count += 1
            if elapsed > 0.1:
                # Confirmed via direct /proc sampling: mpg123 goes idle in
                # anon_pipe_read (starved) for several seconds at a time,
                # then car_sim.py blocks in anon_pipe_write (backlog burst)
                # right after -- i.e. the READER stalls, not the audio
                # pipeline. straddle_count/attempts alone don't show WHICH
                # individual read balloons or why; this does, immediately
                # and unbatched (unlike the once-per-second reads][timing]
                # summary below, which can hide a single multi-second read
                # inside an otherwise-normal-looking 1-second bucket).
                print(f"[s3][SLOW_READ] lba={lba} count={count} attempts={attempts}/"
                      f"{MAX_READ_RETRIES} elapsed={elapsed*1000:.1f}ms", file=sys.stderr)
            read_time_total += elapsed
            read_count += 1
            conn.sendall(data)

            now = time.monotonic()
            if now - last_log >= 1.0:
                print(f"[s3][reads][timing] t={now - start:6.2f}s  reads={read_count:6d}  "
                      f"straddles_avoided={straddle_count:4d}  torn_served={torn_count:3d}",
                      file=sys.stderr)
                last_log = now
    except ConnectionError:
        print("[s3] radio disconnected", file=sys.stderr)
    if read_count:
        print(f"[s3] served {read_count} READ10 requests, "
              f"avg handler latency {read_time_total / read_count * 1000:.3f}ms, "
              f"{straddle_count} straddles avoided, {torn_count} torn reads served anyway",
              file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--expected-serial", default=EXPECTED_SERIAL,
                     help="serial number of the correct board -- the ACTUAL /dev/ttyACMx path "
                          "is now resolved automatically and re-resolved on every reconnect "
                          "(a fixed path was a real, confirmed problem: it can change on any "
                          "USB re-enumeration, e.g. after the ESP32 crashes and reboots, "
                          "silently orphaning a hardcoded connection with no error -- see "
                          "run_serial_bridge()). NEVER pass 5B07008126 (Fin-ESP, a different, "
                          "unrelated live board) here.")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--radio-port", type=int, default=9401, help="port the radio connects to")
    ap.add_argument("--capacity-mb", type=float, default=0.4578,
                     help="ring buffer size -- this bounds worst-case catch-up lag, NOT total "
                          "recording length. 0.4578MB is ~30s of audio at 128kbps (117 clusters). "
                          "REDUCED from an intermediate 5.0MB/~5.46min default after real "
                          "live-phone testing showed that size was a real, worse-in-practice "
                          "problem than the one it was fixing: because the reader's wrap-to-start "
                          "cycle runs on its own clock, completely independent of the phone's "
                          "actual playback position (pause, seek, whatever), a big ring means "
                          "that whenever the reader's cycle DOES wrap, it can replay content from "
                          "up to the FULL ring duration ago -- confirmed directly: pausing and "
                          "unpausing produced audio jumping back to a much-earlier point in the "
                          "song, disorienting in exactly the way a real listener notices "
                          "immediately, worse than the milder ~130ms wrap-splice skip the 5.0MB "
                          "size was fixing. 30s keeps that worst-case 'how stale can replayed "
                          "content be' window short enough to be barely noticeable, while still "
                          "being ~2.3x the ORIGINAL problem size (0.2MB/~13s) that first exposed "
                          "the wrap-splice bug, so wrap-splice frequency is still meaningfully "
                          "better than that original worst case, just not as aggressively fixed "
                          "as the intermediate 5.0MB attempt was. This is a real, live tradeoff "
                          "(splice-frequency vs. stale-replay-window) with no size that eliminates "
                          "both -- 30s is a deliberate re-balancing toward what live testing showed "
                          "actually matters more for a real listener. 117 clusters is comfortably "
                          "under the 4085-cluster FAT12 ceiling in fat12_disk.py's "
                          "`assert self.data_clusters < 4085`. Ring size is NOT the dominant "
                          "factor in end-to-end latency (measured directly: shrinking it all the "
                          "way to ~1.0s of audio barely moved the real number -- the actual "
                          "~9.5-10.6s floor is inherent to MP3 decode-side buffering on a "
                          "slowly-arriving stream, confirmed by a raw-PCM control test showing "
                          "~2.3s under the identical feed pattern). What ring size also affects: "
                          "READ_SAFETY_MARGIN below needs the writer to clear a fixed 16 "
                          "cluster-widths (65536 bytes) past the read before serving it, and that "
                          "margin is a fixed absolute size -- on a too-small ring (confirmed "
                          "directly: a 12-cluster ring made a 2-cluster margin shadow a quarter "
                          "of the whole ring at any given snapshot) that leaves almost no slack "
                          "against ordinary timing jitter, which is exactly what produced real, "
                          "reproducible ~3.5s hard-mute periods every ~11s. At this 0.4578MB/117-"
                          "cluster size the 65536-byte margin is ~13.7 percent of the ring -- more "
                          "comfortable than the original 0.2MB ring's ~31.4 percent, so this "
                          "reduction does NOT reintroduce the old too-tight-margin problem. NOTE: "
                          "the FAT chain terminates at this capacity -- a reader that doesn't "
                          "repeat-on-EOF will stop after one pass; see STATUS.md")
    args = ap.parse_args()

    disk = GrowingFat12Disk(capacity_bytes=int(args.capacity_mb * 1024 * 1024))
    print(f"[s3] FAT12 volume: {disk.total_sectors} sectors, "
          f"declared file size {disk.declared_file_size} bytes, "
          f"first data LBA {disk.first_data_lba}", file=sys.stderr)

    threading.Thread(target=run_serial_bridge,
                      args=(args.expected_serial, args.baud, disk),
                      daemon=True).start()

    radio_srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    radio_srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    radio_srv.bind(("127.0.0.1", args.radio_port))
    radio_srv.listen(1)
    print(f"[s3] waiting for radio on port {args.radio_port}...", file=sys.stderr)

    while True:
        radio_conn, _ = radio_srv.accept()
        print("[s3] radio connected", file=sys.stderr)
        serve_radio(radio_conn, disk)


if __name__ == "__main__":
    main()
