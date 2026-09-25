#!/usr/bin/env bash
# Build and flash the ESP32-S3 with the car build's flags. THE place the S3's
# flags live -- don't retype them by hand (a dropped flag has cost hours
# twice: CLAUDE.md "Build/flash commands").
#
#   ./flash.sh                 build + flash the car build
#   ./flash.sh --trace         + MSC_TRACE (log every SCSI command; car capture)
#   ./flash.sh --v1             the v1 build instead of v2 (see BUILD_FLAGS.md)
#   ./flash.sh --no-upload     compile only
#   ./flash.sh -DSOME_FLAG     extra -D flags, appended
#
# The firmware prints its flags and git commit at boot ("[s3] build: ..."),
# and each flash is appended to logs/flash_history.log.
set -euo pipefail
cd "$(dirname "$0")"

# Car build (verified in the Kenwood 2026-09-25). ENCODE_ON_S3 must match
# the classic's build (esp32-bt-mp3-test/flash.sh).
FLAGS=(
  -DFATDISK_ALWAYS_SERVE_LIVE  # serve reads from the live edge, not the requested offset
  -DFATDISK_MULTI_FILE         # 3 files on the same live stream + Next/Back relay to the phone
  -DLED_RAINBOW_PLAYING        # rainbow LED while playing
  -DENCODE_ON_S3               # the S3 runs the MP3 encoder; classic sends PCM at 2 Mbaud
)

SERIAL=${S3_SERIAL:-5CE5146685}  # USB serial number of the board to flash
PORT=/dev/serial/by-id/usb-1a86_USB_Single_Serial_${SERIAL}-if00
FQBN="esp32:esp32:esp32s3:USBMode=default,PSRAM=opi"
UPLOAD=1
TRACE=0
for a in "$@"; do
  case $a in
    # v1, exactly as car-proven 2026-09-18: one file served by requested offset, 25.6 s ring
    --v1) FLAGS=(-DFATDISK_DATA_CLUSTERS=100) ;;
    --no-upload) UPLOAD=0 ;;
    --trace) TRACE=1 ;;
    -D*) FLAGS+=("$a") ;;
    *) echo "unknown argument: $a" >&2; exit 2 ;;
  esac
done

W=""
if [[ $TRACE == 1 ]]; then
  FLAGS+=(-DMSC_TRACE)
  for s in usbd_edpt_xfer usbd_edpt_stall mscd_xfer_cb mscd_reset mscd_control_xfer_cb \
           tud_descriptor_device_cb tud_descriptor_configuration_cb tud_descriptor_string_cb; do
    W+=" -Wl,--wrap=$s"
  done
fi

SHA=$(git rev-parse --short=7 HEAD)
DIRTY=0; git diff --quiet HEAD -- . ../common || DIRTY=1
F="${FLAGS[*]} -DBUILD_GIT_SHA=0x$SHA -DBUILD_GIT_DIRTY=$DIRTY"

echo "S3 flags: ${FLAGS[*]}  (commit $SHA$([[ $DIRTY == 1 ]] && echo +dirty))"
arduino-cli compile --fqbn "$FQBN" \
  --build-property "compiler.cpp.extra_flags=$F" --build-property "compiler.c.extra_flags=$F" \
  --build-property "compiler.c.elf.extra_flags=$W" .
[[ $UPLOAD == 1 ]] || exit 0

[[ -e $PORT ]] || { echo "S3 ($SERIAL) not connected: $PORT missing" >&2; exit 1; }
# Anchored: only a process whose command line STARTS with python ... serial_logger.py
# matches, never a shell whose command line merely mentions it (pkill -f self-match).
pkill -f "^[^ ]*python3?(\.[0-9]+)? [^ ]*serial_logger\.py --by-id $PORT" && sleep 0.5 || true
arduino-cli upload -p "$PORT" --fqbn "$FQBN" .
mkdir -p ../logs
echo "$(date '+%F %T') S3 $SHA$([[ $DIRTY == 1 ]] && echo +dirty) ${FLAGS[*]}" >> ../logs/flash_history.log
echo "flashed S3 $SERIAL ($(readlink -f "$PORT")); its serial logger was stopped -- restart it"
