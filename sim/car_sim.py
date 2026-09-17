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
import socket
import subprocess
import sys

from sector_protocol import SECTOR_SIZE, read_sectors

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
    }


def find_file_entry(root_dir_bytes):
    for i in range(0, len(root_dir_bytes), 32):
        entry = root_dir_bytes[i:i + 32]
        if entry[0] in (0x00, 0xE5):
            continue
        attr = entry[11]
        if attr & 0x08:  # volume label
            continue
        name = entry[0:11].decode("ascii", errors="replace").strip()
        first_cluster = int.from_bytes(entry[26:28], "little")
        size = int.from_bytes(entry[28:32], "little")
        return {"name": name, "first_cluster": first_cluster, "size": size}
    return None


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
    args = ap.parse_args()

    print(f"[radio] connecting to board on port {args.port}...", file=sys.stderr)
    sock = socket.create_connection(("127.0.0.1", args.port))
    # A stuck request has no reason to hang forever, and an unbounded
    # sock.recv() has no way to fail loudly if the server-side does.
    # Confirmed directly: this hung silently for the rest of a real
    # session once, with no exception, no log line, no visible cause.
    sock.settimeout(15.0)
    print("[radio] connected, reading boot sector...", file=sys.stderr)

    player = None
    capture_f = None
    total_read = 0
    try:
        boot = read_sectors(sock, 0, 1)
        layout = parse_boot_sector(boot)
        print(f"[radio] parsed BPB: {layout}", file=sys.stderr)

        fat = read_sectors(sock, layout["reserved_sectors"], layout["fat_size_sectors"])

        root_dir = read_sectors(sock, layout["root_dir_lba"], layout["root_dir_sectors"])
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
        cluster_pos = 0
        while True:
            cluster = cluster_list[cluster_pos]
            lba = layout["data_lba"] + (cluster - 2) * layout["sectors_per_cluster"]
            data = read_sectors(sock, lba, layout["sectors_per_cluster"])
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
