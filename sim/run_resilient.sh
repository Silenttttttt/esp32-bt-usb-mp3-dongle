#!/usr/bin/env bash
# Single entry point to start the full PC-side pipeline (s3_sim_serial.py +
# car_sim.py) with BOTH processes wrapped in their own forever-restart
# loops, for real unattended use (e.g. actually driving, with no way to
# physically intervene if something crashes).
#
# WHY THIS EXISTS: car_sim.py already had its own restart-forever wrapper
# used ad hoc all session, but s3_sim_serial.py did not -- it was launched
# as a bare command each time, so ANY unhandled crash of the whole process
# (not just the serial connection, which is now separately self-healing
# via run_serial_bridge() inside the script itself) would have silently
# killed the entire pipeline with nothing to bring it back. This script
# makes BOTH processes equally resilient with one command.
#
# Usage: ./run_resilient.sh
# Stop with Ctrl-C (kills both wrapper loops and their children).

set -u
cd "$(dirname "$0")"

cleanup() {
  echo "[run_resilient] stopping..." >&2
  kill 0 2>/dev/null
}
trap cleanup EXIT INT TERM

(
  while true; do
    python3 s3_sim_serial.py --radio-port 9401
    echo "[run_resilient] s3_sim_serial.py exited, restarting in 1s..." >&2
    sleep 1
  done
) &

(
  while true; do
    python3 car_sim.py --port 9401
    echo "[run_resilient] car_sim.py exited, restarting in 1s..." >&2
    sleep 1
  done
) &

wait
