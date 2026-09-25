#!/usr/bin/env bash
# Build and flash the classic ESP32 with the car build's flags. THE place the
# classic's flags live -- don't retype them by hand.
#
#   ./flash.sh              build + flash
#   ./flash.sh --no-upload  compile only
#   ./flash.sh -DSOME_FLAG  extra -D flags, appended
#
# Each flash is appended to logs/flash_history.log; the firmware sends its
# flags and git commit at boot as a "BUILD:" control frame.
set -euo pipefail
cd "$(dirname "$0")"

FLAGS=(
  -DINT2IDX_SIZE=4000   # Shine encoder quantization table: 4000 not 10000 entries (~24 KB RAM); only used without ENCODE_ON_S3
  -DDIAG_LOOP_DRAIN     # load-bearing: Shine/PCM work stays in loop(); a separate task breaks BT
  -DDIAG_FRAG_TRACE     # FRAG: heap lines once a second
  -DV2_ALL              # AVRCP features: radio button relay, auto resume/skip. Never disable AVRCP
  -DENCODE_ON_S3        # send mono PCM at 2 Mbaud; the S3 encodes. Must match esp32-s3-msc/flash.sh
)

SERIAL=5B52096812
PORT=/dev/serial/by-id/usb-1a86_USB_Single_Serial_${SERIAL}-if00
FQBN=esp32:esp32:esp32
UPLOAD=1
for a in "$@"; do
  case $a in
    --no-upload) UPLOAD=0 ;;
    -D*) FLAGS+=("$a") ;;
    *) echo "unknown argument: $a" >&2; exit 2 ;;
  esac
done

SHA=$(git rev-parse --short=7 HEAD)
DIRTY=0; git diff --quiet HEAD -- . ../common || DIRTY=1
F="${FLAGS[*]} -DBUILD_GIT_SHA=0x$SHA -DBUILD_GIT_DIRTY=$DIRTY"

echo "classic flags: ${FLAGS[*]}  (commit $SHA$([[ $DIRTY == 1 ]] && echo +dirty))"
arduino-cli compile --fqbn "$FQBN" \
  --build-property "compiler.c.extra_flags=$F" --build-property "compiler.cpp.extra_flags=$F" .
[[ $UPLOAD == 1 ]] || exit 0

[[ -e $PORT ]] || { echo "classic ($SERIAL) not connected: $PORT missing" >&2; exit 1; }
# Anchored: only a process whose command line STARTS with python ... serial_logger.py
# matches, never a shell whose command line merely mentions it (pkill -f self-match).
pkill -f "^[^ ]*python3?(\.[0-9]+)? [^ ]*serial_logger\.py --by-id $PORT" && sleep 0.5 || true
arduino-cli upload -p "$PORT" --fqbn "$FQBN" .
mkdir -p ../logs
echo "$(date '+%F %T') classic $SHA$([[ $DIRTY == 1 ]] && echo +dirty) ${FLAGS[*]}" >> ../logs/flash_history.log
echo "flashed classic $SERIAL ($(readlink -f "$PORT")); its serial logger was stopped -- restart it"
