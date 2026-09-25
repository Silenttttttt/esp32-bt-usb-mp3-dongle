#!/usr/bin/env bash
# Same role as run_resilient_real_firmware.sh, but built with
# -DFATDISK_MULTI_FILE (3 files, all aliasing the same live ring) and
# car_sim.py launched with --gui, to test PLAN_NEXT.md's C3 idea end-to-end
# against the real classic ESP32: does clicking Next/Back in the GUI window
# get detected server-side and relayed as a real "next"/"prev" command?
#
# Deliberately a SEPARATE script/binary from run_resilient_real_firmware.sh
# (s3_real_firmware_host_multi, not s3_real_firmware_host) -- the real
# single-file production path stays completely untouched either way.
#
# Usage: ./run_multi_file_gui.sh
# Stop with Ctrl-C, or just close the GUI window (the reader thread stops on
# its own once the socket closes).
#
# Same caution as run_resilient_real_firmware.sh: don't run this at the same
# time as run_resilient.sh or run_resilient_real_firmware.sh -- they'd all
# fight over the same real classic-ESP32 serial port.

set -u
cd "$(dirname "$0")"

g++ -O2 -std=c++17 -pthread -Wall -Wextra -DFATDISK_MULTI_FILE \
  -o s3_real_firmware_host_multi s3_real_firmware_host.cpp || {
  echo "[run_multi_file_gui] build failed, aborting" >&2
  exit 1
}

cleanup() {
  echo "[run_multi_file_gui] stopping..." >&2
  trap - EXIT INT TERM
  kill 0 2>/dev/null
}
trap cleanup EXIT INT TERM

./s3_real_firmware_host_multi --radio-port 9491 &

python3 car_sim.py --gui --port 9491

# GUI window closed -- tear down the S3 stand-in too instead of leaving it
# running headless in the background.
kill 0 2>/dev/null
