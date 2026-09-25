#!/usr/bin/env bash
# Build and flash the S3 with the car-session MSC_TRACE build (msc_trace.h),
# with every flag the current build needs, so none gets dropped by hand.
# Extra -D flags go on the command line, e.g.:
#   ./flash_trace.sh -DEARLY_END_READ_ERROR
#   ./flash_trace.sh --no-upload          (compile only)
# Stops the S3's serial logger first; restart it with logs/car_capture.sh start.
set -euo pipefail
cd "$(dirname "$0")"

SERIAL=5CE5146685
PORT=/dev/serial/by-id/usb-1a86_USB_Single_Serial_${SERIAL}-if00
FQBN="esp32:esp32:esp32s3:USBMode=default,PSRAM=opi"
UPLOAD=1
EXTRA=()
for a in "$@"; do
  if [[ $a == --no-upload ]]; then UPLOAD=0; else EXTRA+=("$a"); fi
done

# Must match the classic's build: ENCODE_ON_S3 on both or neither (CLAUDE.md).
F="-DFATDISK_ALWAYS_SERVE_LIVE -DFATDISK_MULTI_FILE -DLED_RAINBOW_PLAYING -DENCODE_ON_S3 -DMSC_TRACE ${EXTRA[*]:-}"
W=""
for s in usbd_edpt_xfer usbd_edpt_stall mscd_xfer_cb mscd_reset mscd_control_xfer_cb \
         tud_descriptor_device_cb tud_descriptor_configuration_cb tud_descriptor_string_cb; do
  W+=" -Wl,--wrap=$s"
done

echo "flags: $F"
arduino-cli compile --fqbn "$FQBN" \
  --build-property "compiler.cpp.extra_flags=$F" --build-property "compiler.c.extra_flags=$F" \
  --build-property "compiler.c.elf.extra_flags=$W" .
[[ $UPLOAD == 1 ]] || exit 0

[[ -e $PORT ]] || { echo "S3 ($SERIAL) not connected: $PORT missing" >&2; exit 1; }
pkill -f "[s]erial_logger.py --by-id $PORT" && sleep 0.5 || true
arduino-cli upload -p "$PORT" --fqbn "$FQBN" .
echo "flashed $SERIAL ($(readlink -f "$PORT"))"
