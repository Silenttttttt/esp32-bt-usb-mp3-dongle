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

echo "[preflight] checking for stale audio/BT processes..." >&2
STALE=$(ps aux | grep -iE "bluealsad|aplay -D|mpris-proxy|s3_sim_serial|car_sim\.py" | grep -v grep || true)

if [ -n "$STALE" ]; then
  echo "[preflight] found processes (will kill all, then restart bluealsad fresh):" >&2
  echo "$STALE" >&2
  echo "$STALE" | awk '{print $2}' | while read -r pid; do
    sudo -n kill -9 "$pid" 2>/dev/null || kill -9 "$pid" 2>/dev/null || true
  done
  sleep 1
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
