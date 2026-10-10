#!/usr/bin/env python3
"""Live-serving cushion over time, from the S3 log: write_pos (newest MP3 byte)
minus the radio's live cursor, per heartbeat. Normal ~12 KB (~0.77 s). Growing =
the S3 is getting more audio than real time (delay grows until a 0.5 s catch-up
jump); shrinking = less (until a 0.77 s underrun).
  tools/s3_lag.py [logs/s3_serial.log] [since "YYYY-MM-DD HH:MM:SS"]"""
import re, sys
path = sys.argv[1] if len(sys.argv) > 1 else "logs/s3_serial.log"
since = sys.argv[2] if len(sys.argv) > 2 else ""
RING = 5570560
wp = None
for line in open(path, errors="replace"):
    t = line[1:20]
    if not line.startswith("[20") or (since and t < since):
        continue
    m = re.search(r"link heartbeat: .*write_pos=(\d+)", line)
    if m:
        wp = int(m.group(1)); continue
    m = re.search(r"live: underruns=(\d+) .*cursor=(\d+)", line)
    if m and wp is not None:
        lag = (wp - int(m.group(2))) % RING
        print(f"{t[11:]} lag={lag:6d} B ({lag/16000:4.2f} s) underruns={m.group(1)}")
