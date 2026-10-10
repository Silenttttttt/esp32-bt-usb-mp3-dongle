#!/usr/bin/env python3
"""Print the S3's event log (LINK_EVENTS): the last 64 unusual things it saw.

  tools/s3_events.py            print the log
  tools/s3_events.py --clear    print it, then clear it on the board

Plug the laptop into the S3's debug (UART) USB-C port. Works with or without
logs/serial_logger.py running on that port: it sends 'E' and reads the reply
(from the port, or from logs/s3_serial.log if the logger owns the port).
Times are "boot N +h:mm:ss": the Nth S3 power-up (about one per drive) and
how long after that power-up it happened.
"""
import os, subprocess, sys, time
import serial

SERIAL = os.environ.get("S3_SERIAL", "5CE5146685")
PORT = f"/dev/serial/by-id/usb-1a86_USB_Single_Serial_{SERIAL}-if00"
LOG = os.path.join(os.path.dirname(__file__), "..", "logs", "s3_serial.log")


def logger_running():
    r = subprocess.run(["pgrep", "-f", f"^[^ ]*python3?(\\.[0-9]+)? [^ ]*serial_logger\\.py --by-id {PORT}"],
                       capture_output=True)
    return r.returncode == 0


def open_port():
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = PORT, 115200, 0.2
    s.dtr = False  # don't toggle the auto-reset lines on open
    s.rts = False
    s.open()
    return s


def main():
    clear = "--clear" in sys.argv
    if not os.path.exists(PORT):
        sys.exit(f"S3 debug port not found: {PORT}")
    lines = []
    if logger_running():
        start = os.path.getsize(LOG)
        with open_port() as s:
            s.write(b"E")
        deadline = time.time() + 5
        while time.time() < deadline:
            with open(LOG, errors="replace") as f:
                f.seek(start)
                got = [l for l in f.read().splitlines() if "[s3] event" in l]
            if any("events end" in l for l in got):
                lines = got
                break
            time.sleep(0.2)
    else:
        with open_port() as s:
            s.reset_input_buffer()
            s.write(b"E")
            deadline = time.time() + 5
            buf = b""
            while time.time() < deadline and b"events end" not in buf:
                buf += s.read(4096)
            lines = [l for l in buf.decode(errors="replace").splitlines() if "[s3] event" in l]
    if not lines:
        sys.exit("no reply (is it a LINK_EVENTS build? flash.sh --fix-link)")
    for l in lines:
        l = l[l.index("[s3]"):]
        if not l.startswith("[s3] events end"):
            print(l.replace("[s3] ", ""))
    if clear:
        with open_port() as s:
            s.write(b"ZZ")
        print("cleared")


main()
