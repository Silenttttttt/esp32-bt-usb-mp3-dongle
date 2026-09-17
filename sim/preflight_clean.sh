#!/usr/bin/env bash
# Pre-flight cleanup: kill any stale bluealsad/aplay/mpris-proxy processes
# before starting a real test session.
#
# WHY THIS EXISTS: this exact class of bug has independently caused two
# separate multi-hour debugging detours in one night. A stale `bluealsad`
# or `aplay` process left running from an EARLIER test session silently
# squats on D-Bus registrations and/or repeatedly races bluealsad's own
# endpoint (re-)registration against a live connection attempt, producing
# symptoms that look exactly like a deep, unfixable BlueZ/Bluedroid bug
# (page timeouts, AVDTP abort loops, even a real bluetoothd
# use-after-free crash triggered by the resulting endpoint churn) but are
# actually just "you forgot a background process was still running."
#
# RULE: run this before trusting ANY "it's unreliable" diagnosis on this
# rig, and before starting any real reliability test.

set -u

# REAL, CONFIRMED BUG (found live during a session, reproduced again here
# via an isolated test): killing only the leaf s3_sim_serial.py/car_sim.py
# processes below does NOT stop run_resilient.sh's own restart-forever
# wrapper loops -- that script's `while true; do python3 ...; done`
# subshells are a SEPARATE process from the python3 child and don't match
# the process-name grep below at all. The instant the leaf gets killed,
# the still-alive wrapper loop notices the child exited and immediately
# launches a fresh one -- often before this script even finishes its own
# "clean now" checks, silently defeating the entire point of a preflight
# reset (confirmed directly: a killed leaf respawned within ~0ms of the
# kill, well before any subsequent step here could run). Fix: kill
# run_resilient.sh's own process FIRST -- it traps INT/TERM and runs
# `kill 0` on its own process group, which (since it never sets `-m`/job
# control, so its background subshells share ONE process group with it)
# cleanly tears down both wrapper loops AND their current children in one
# shot, before this script does anything else.
echo "[preflight] stopping any run_resilient.sh wrapper first (it would otherwise "
echo "  instantly respawn anything killed below)..." >&2
if pkill -TERM -f "run_resilient\.sh" 2>/dev/null; then
  sleep 1
  pkill -9 -f "run_resilient\.sh" 2>/dev/null || true
  sleep 1
fi

echo "[preflight] checking for stale audio/BT processes..." >&2
STALE=$(ps aux | grep -iE "bluealsad|aplay -D|mpris-proxy|s3_sim_serial|car_sim\.py" | grep -v grep || true)

if [ -n "$STALE" ]; then
  echo "[preflight] found processes (will kill all, then restart bluealsad fresh):" >&2
  echo "$STALE" >&2
  echo "$STALE" | awk '{print $2}' | while read -r pid; do
    sudo -n kill -9 "$pid" 2>/dev/null || kill -9 "$pid" 2>/dev/null || true
  done
  sleep 1
  # Confirm nothing respawned (e.g. from a run_resilient.sh instance this
  # script didn't know how to name-match, or any other wrapper) -- if
  # something's still here, the caller needs to know the "clean" state
  # this script promises wasn't actually achieved.
  STILL_ALIVE=$(ps aux | grep -iE "s3_sim_serial|car_sim\.py" | grep -v grep || true)
  if [ -n "$STILL_ALIVE" ]; then
    echo "[preflight] WARNING: processes respawned after kill -- a wrapper this "
    echo "  script doesn't know about may still be running:" >&2
    echo "$STILL_ALIVE" >&2
  fi
else
  echo "[preflight] no stale processes found" >&2
fi

echo "[preflight] restarting bluetoothd + bluealsad fresh..." >&2
sudo -n systemctl restart bluetooth 2>/dev/null || {
  echo "[preflight] ERROR: sudo needs a cached timestamp for 'systemctl restart bluetooth' -- run 'sudo -v' or add a NOPASSWD rule" >&2
  exit 2
}
sleep 2
sudo -n bash -c 'nohup bluealsad -p a2dp-source -p a2dp-sink --all-codecs > /tmp/bluealsad_preflight.log 2>&1 & disown'
sleep 2

if systemctl is-active --quiet bluetooth; then
  echo "[preflight] clean -- bluetoothd active, bluealsad restarted" >&2
  exit 0
else
  echo "[preflight] WARNING: bluetoothd not active after restart" >&2
  exit 1
fi
