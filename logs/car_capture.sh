#!/usr/bin/env bash
# Car capture session control (2026-09-25). Logs go to logs/car/<session>/.
#   car_capture.sh start [name]   start both loggers (S3 trace + classic), new session dir
#   car_capture.sh mark <text>    timestamped marker in the S3 log ("pressed Next", ...)
#   car_capture.sh replay         S3 re-prints its first 4096 trace records since power-on
#   car_capture.sh status         loggers running? last trace lines
#   car_capture.sh stop           stop both loggers
# Ports are picked by USB serial number, never by ttyACM number.
set -euo pipefail
cd "$(dirname "$0")/.."

PY=/usr/bin/python3  # has pyserial (the pyenv python3 doesn't)
S3=/dev/serial/by-id/usb-1a86_USB_Single_Serial_5CE5146685-if00
CL=/dev/serial/by-id/usb-1a86_USB_Single_Serial_5B52096812-if00
CUR=logs/car/current

stop_loggers() {
  pkill -f "[s]erial_logger.py --by-id $S3" || true
  pkill -f "[s]erial_logger.py --by-id $CL" || true
}

case "${1:-}" in
  start)
    stop_loggers
    dir="logs/car/$(date +%Y%m%d_%H%M%S)${2:+_$2}"
    mkdir -p "$dir"
    ln -sfn "$(basename "$dir")" "$CUR"
    nohup $PY logs/serial_logger.py --by-id "$S3" --baud 921600 --mode line --out "$dir/s3.log" >/dev/null 2>&1 &
    nohup $PY logs/serial_logger.py --by-id "$CL" --baud 2000000 --mode framed --out "$dir/classic.log" >/dev/null 2>&1 &
    echo "logging to $dir (s3.log, classic.log)"
    ;;
  mark)
    shift
    line="[$(date '+%Y-%m-%d %H:%M:%S.%3N')] ===== MARK: $* ====="
    echo "$line" >> "$CUR/s3.log"
    echo "$line" >> "$CUR/classic.log"
    echo "$line"
    ;;
  replay)
    $PY -c "import serial; s=serial.Serial('$S3', 921600); s.write(b'B'); s.close()"
    echo "replay requested; see $CUR/s3.log"
    ;;
  status)
    pgrep -af "[s]erial_logger.py" || echo "no loggers running"
    [[ -e $CUR/s3.log ]] && grep -a " T [0-9]" "$CUR/s3.log" | tail -5
    ;;
  stop)
    stop_loggers
    echo stopped
    ;;
  *)
    sed -n '2,8p' "$0"
    exit 1
    ;;
esac
