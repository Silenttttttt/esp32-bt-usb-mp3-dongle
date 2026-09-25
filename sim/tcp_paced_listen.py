#!/usr/bin/env python3
"""Throwaway diagnostic: a properly-paced (matches real_s3_listen.py's own
16000 B/s pacing) TCP-based reader for testing FATDISK_ALWAYS_SERVE_LIVE
against the PC-hosted stand-in (s3_real_firmware_host_live), before ever
flashing this new, unverified design to the real S3 board. car_sim.py's
own --port path has ZERO pacing (backpressure-only), which is EXACTLY the
failure mode FATDISK_ALWAYS_SERVE_LIVE cannot tolerate (confirmed live,
2026-09-20: constant "Header missing" garbage). real_s3_listen.py paces
correctly but only talks to a real block device via UDisks2, not a TCP
stand-in. This bridges that gap for stand-in testing only.

Usage: python3 tcp_paced_listen.py [port]
"""
import subprocess
import sys
import time

from sector_protocol import read_sectors

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9601
SECTOR_SIZE = 512
BITRATE_BYTES_PER_SEC = 16000
READ_CHUNK = 4096

import socket

sock = socket.create_connection(("127.0.0.1", PORT))
sock.settimeout(15.0)

boot = read_sectors(sock, 0, 1)

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from car_sim import parse_boot_sector

layout = parse_boot_sector(boot)
print(f"[tcp-paced] layout: {layout}", file=sys.stderr)

player = subprocess.Popen(["mpg123", "--resync-limit", "-1", "-"], stdin=subprocess.PIPE)

pos = 0
next_send_time = time.monotonic()
try:
    while True:
        cluster_sectors = layout["sectors_per_cluster"]
        cluster_size = cluster_sectors * SECTOR_SIZE
        cluster_idx = pos // cluster_size
        lba = layout["data_lba"] + cluster_idx * cluster_sectors
        chunk = read_sectors(sock, lba, cluster_sectors)
        try:
            player.stdin.write(chunk)
            player.stdin.flush()
        except BrokenPipeError:
            print("[tcp-paced] player exited", file=sys.stderr)
            break
        pos = (pos + cluster_size) % (938 * cluster_size)
        next_send_time += cluster_size / BITRATE_BYTES_PER_SEC
        sleep_for = next_send_time - time.monotonic()
        if sleep_for > 0:
            time.sleep(sleep_for)
        else:
            next_send_time = time.monotonic()
except KeyboardInterrupt:
    print("\n[tcp-paced] stopped", file=sys.stderr)
finally:
    try:
        player.stdin.close()
    except Exception:
        pass
    player.terminate()
