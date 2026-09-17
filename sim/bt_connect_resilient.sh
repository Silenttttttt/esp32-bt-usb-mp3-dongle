#!/usr/bin/env bash
# Resilient connect-and-hold script for the ESP32 A2DP sink test device.
#
# WHY THIS EXISTS: BlueZ 5.87 (confirmed even in a from-git-master build, so
# this is a genuine unfixed upstream bug, not something patchable from our
# side) has a real use-after-free in profiles/audio/avdtp.c's abort/reconfig
# path that crashes bluetoothd on a meaningful fraction of AVDTP negotiation
# attempts -- confirmed via direct bluetoothd -d debug capture: bluealsad
# dynamically registers an extra MediaEndpoint1 object mid-negotiation
# (e.g. "A2DP/SBC/source/3" appearing while a SetConfiguration for an
# existing endpoint is still in flight), and BlueZ's AVDTP session state
# machine does not handle that gracefully -- it transitions straight to
# ABORTING, double-frees a setup struct, and bluetoothd SIGABRTs. This is
# NOT fixable from our own code (not our bug, not ESP32/Bluedroid's bug,
# and not something a bluealsad config flag avoids -- tested -c SBC,
# -p a2dp-source alone, still reproduces).
#
# Real-world context: this only manifests during CONNECTION ESTABLISHMENT/
# renegotiation. Once a connection is up and streaming, it has run stable
# for 500+ seconds in testing with zero further issue. A real deployment
# (phone in a car) establishes the connection once when the car starts and
# holds it for the whole drive -- it does not repeatedly reconnect the way
# aggressive testing does. So the practical fix is: retry automatically
# until one attempt succeeds (bluetoothd recovers in ~2s after a crash),
# rather than trying to eliminate the underlying bug.
#
# Usage: ./bt_connect_resilient.sh <MAC> [max_attempts]

set -u
MAC="${1:?usage: bt_connect_resilient.sh <MAC> [max_attempts]}"
MAX_ATTEMPTS="${2:-20}"

daemon_active() {
  systemctl is-active --quiet bluetooth
}

restart_daemon() {
  echo "[resilient] restarting bluetoothd..." >&2
  # Requires a cached sudo timestamp (run `sudo -v` before this script, or
  # add a NOPASSWD sudoers rule for this exact command) -- never hardcode
  # credentials in this file.
  sudo -n systemctl restart bluetooth || {
    echo "[resilient] ERROR: sudo needs a cached timestamp (run 'sudo -v' first) or a NOPASSWD rule for 'systemctl restart bluetooth'" >&2
    exit 2
  }
  sleep 2
}

for attempt in $(seq 1 "$MAX_ATTEMPTS"); do
  if ! daemon_active; then
    restart_daemon
  fi

  result=$(timeout 15 bluetoothctl connect "$MAC" 2>&1)
  echo "[resilient] attempt $attempt: $(echo "$result" | tail -1)" >&2

  if echo "$result" | grep -q "Connection successful"; then
    echo "[resilient] connected successfully on attempt $attempt" >&2
    exit 0
  fi

  if ! daemon_active; then
    echo "[resilient] bluetoothd crashed on this attempt, will retry" >&2
    restart_daemon
  fi

  sleep 1
done

echo "[resilient] FAILED to connect after $MAX_ATTEMPTS attempts" >&2
exit 1
