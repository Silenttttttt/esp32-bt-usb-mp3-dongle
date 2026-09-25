#!/usr/bin/env python3
"""Tiny, root-only privileged helper: opens a block device with O_RDONLY |
O_DIRECT (needs root -- the real S3 device node is root:disk-owned) and
hands the resulting fd back to the calling (NON-root) parent process over
a Unix domain socket via SCM_RIGHTS fd-passing.

Exists specifically to avoid two real, confirmed problems found tonight
(2026-09-21) with the alternatives:
- Plain `os.open()` needs root -- but running the WHOLE test script
  (mpg123 included) as root broke real PulseAudio/PipeWire output
  (confirmed live: "didn't even play the right song, played for a sec" --
  a root-owned audio client gets its stream silently dropped).
- UDisks2's OpenDevice() D-Bus call is passwordless for an active local
  session in some configurations, but triggered an interactive polkit
  password prompt in this one -- not the fully-automated flow needed.

This script does ONLY the one thing that needs root (open the fd), then
immediately exits after handing it off -- the parent process (this file's
caller) keeps running as the normal user throughout, so mpg123's audio
output is correct.

Usage (not meant to be run directly -- see car_sim.py's
HelperDeviceTransport for the actual invocation):
    python3 open_device_fd_helper.py <device_path> <unix_socket_path>
"""
import os
import socket
import sys

O_DIRECT = 0o40000

device_path = sys.argv[1]
socket_path = sys.argv[2]

fd = os.open(device_path, os.O_RDONLY | O_DIRECT)

conn = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
conn.connect(socket_path)
socket.send_fds(conn, [b"ok"], [fd])
conn.close()
os.close(fd)
