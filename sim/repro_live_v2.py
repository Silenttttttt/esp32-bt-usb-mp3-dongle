#!/usr/bin/env python3
"""Isolated gui_read_loop() reproduction, bypassing Tkinter entirely, against
the REAL S3 hardware. Throwaway diagnostic, not a shipped tool.

REAL SIMPLIFICATION (2026-09-21): this used to go through a TCP port via
real_s3_passthrough.py (a separate background process bridging the real
block device to a fake TCP wire protocol). That layer's ONLY real purpose
was passwordless read access via UDisks2's OpenDevice() D-Bus call -- the
TCP port itself was just an incidental side effect of it being a separate
bridging process, not something inherently needed. Switched to
UdisksDeviceTransport: the SAME real, direct block-device reads, the SAME
passwordless UDisks2 access, but called INLINE, no port, no separate
process, no root.

REAL BUG FOUND AND FIXED, TWICE (2026-09-21): first tried plain
DeviceTransport+sudo -- running the WHOLE script as root broke real audio
output ("didn't even play the right song, played for a sec", matching
PipeWire/PulseAudio dropping a root-owned client's stream, even with
`sudo -E` preserving PULSE_SERVER/XDG_RUNTIME_DIR). Then tried
UdisksDeviceTransport (passwordless D-Bus OpenDevice(), like
real_s3_passthrough.py has used all along) -- but on this system it
triggered an interactive polkit password PROMPT, not actually
passwordless, breaking automated repeat testing. Fixed with
HelperDeviceTransport: a tiny, separate, SHORT-LIVED root-only helper
process opens just the device fd (the one thing that needs root) and
hands it back via Unix-socket fd-passing, automated with the user's own
sudo password (see HelperDeviceTransport's own docstring) -- everything
else, mpg123 included, stays running as the normal user throughout.

Usage: python3 repro_live_v2.py [device] [duration_s] [bursty: 0|1]
Fully automated -- no interactive prompt of any kind. Pass bursty=1 to
stress-test the THIRD real bug fix (2026-09-21, the ahead-direction
cursor-drift correction) -- see car_sim.py's own --bursty flag /
BURST_AHEAD_SECONDS comment for why the default smooth pacing can never
exercise that path at all.
"""
import subprocess
import sys
import threading
import time

sys.path.insert(0, "./sim")
from car_sim import (
    parse_boot_sector, find_all_file_entries, walk_cluster_chain,
    RadioGuiState, gui_read_loop, HelperDeviceTransport, SUDO_PASSWORD,
)

DEVICE = sys.argv[1] if len(sys.argv) > 1 else "/dev/sda"
DURATION = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
BURSTY = bool(int(sys.argv[3])) if len(sys.argv) > 3 else False
CAPTURE_PATH = sys.argv[4] if len(sys.argv) > 4 else None
NO_PLAY = len(sys.argv) > 5 and sys.argv[5] == "1"

capture_f = open(CAPTURE_PATH, "wb") if CAPTURE_PATH else None

transport = HelperDeviceTransport(DEVICE, SUDO_PASSWORD)

boot = transport.read(0, 1)
layout = parse_boot_sector(boot)
print(f"[repro] layout: {layout}", file=sys.stderr)

fat = transport.read(layout["reserved_sectors"], layout["fat_size_sectors"])
root_dir = transport.read(layout["root_dir_lba"], layout["root_dir_sectors"])
entries = find_all_file_entries(root_dir)
print(f"[repro] found {len(entries)} file entries", file=sys.stderr)

files = []
for e in entries:
    cluster_list = walk_cluster_chain(fat, e["first_cluster"], layout["fat_type"])
    files.append({"name": e["name"], "cluster_list": cluster_list})
    print(f"[repro]   {e['name']}: {len(cluster_list)} clusters", file=sys.stderr)

state = RadioGuiState(files)

player = None if NO_PLAY else subprocess.Popen(
    ["mpg123", "--resync-limit", "-1", "-"], stdin=subprocess.PIPE)

t = threading.Thread(
    target=gui_read_loop,
    args=(transport, layout, state, player, capture_f, layout["volume_serial"], BURSTY),
    daemon=True,
)
t.start()

print(f"[repro] bursty_pacing={BURSTY} no_play={NO_PLAY}", file=sys.stderr)
time.sleep(DURATION)
print(f"[repro] stopping after {DURATION}s", file=sys.stderr)
if player:
    player.terminate()
    try:
        player.wait(timeout=3)
    except subprocess.TimeoutExpired:
        player.kill()
if capture_f:
    capture_f.close()
