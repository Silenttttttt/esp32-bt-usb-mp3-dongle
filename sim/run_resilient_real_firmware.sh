#!/usr/bin/env bash
# Same role as run_resilient.sh, but with s3_sim_serial.py (the old,
# hand-copied Python simulator) replaced by s3_real_firmware_host -- a
# PC-hosted program that #includes and runs the REAL esp32-s3-msc.ino's
# actual disk logic (fat_disk_shared.h), until the physical ESP32-S3
# board arrives. See esp32-s3-msc/fat_disk_shared.h and
# sim/s3_real_firmware_host.cpp's own header comments for the full
# rationale. car_sim.py (the mock car radio) is unchanged and unaware of
# the swap -- it speaks the same sector_protocol.py wire format either way.
#
# Usage: ./run_resilient_real_firmware.sh
# Stop with Ctrl-C (kills both wrapper loops and their children).
#
# Do NOT run this at the same time as run_resilient.sh -- both would try
# to open the same real classic-ESP32 serial port concurrently, stealing
# bytes from each other's read() calls (confirmed directly: a standalone
# test run of s3_real_firmware_host while run_resilient.sh's
# s3_sim_serial.py was already live measurably competed for the same
# incoming UART stream).

set -u
cd "$(dirname "$0")"

g++ -O2 -std=c++17 -pthread -Wall -Wextra -o s3_real_firmware_host s3_real_firmware_host.cpp || {
  echo "[run_resilient_real_firmware] build failed, aborting" >&2
  exit 1
}

cleanup() {
  echo "[run_resilient_real_firmware] stopping..." >&2
  trap - EXIT INT TERM
  kill 0 2>/dev/null
}
trap cleanup EXIT INT TERM

(
  while true; do
    ./s3_real_firmware_host --radio-port 9401
    echo "[run_resilient_real_firmware] s3_real_firmware_host exited, restarting in 1s..." >&2
    sleep 1
  done
) &

(
  while true; do
    python3 car_sim.py --port 9401
    echo "[run_resilient_real_firmware] car_sim.py exited, restarting in 1s..." >&2
    sleep 1
  done
) &

wait
