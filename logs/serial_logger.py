#!/usr/bin/env python3
"""Persistent background logger for one of the two boards' debug serial
ports. Runs forever (until killed), auto-reconnects across device
disappear/re-enumerate (a real, frequent event during crash-loop testing --
confirmed via dmesg re-enumeration on the classic board), and writes
timestamped, flushed-per-line output so `tail -f` always shows live data
and nothing is lost if the process or the board itself restarts.

Two modes:
  --mode line   Plain line-based text console (the S3's USB CDC debug
                Serial, 115200 baud -- pure text, no binary audio mixed in).
  --mode framed Classic board's Serial (921600 baud) uses a custom wire
                protocol (1-byte magic 0xAA + 1-byte type + 4-byte
                big-endian length + payload) to interleave raw MP3 audio
                ('A' frames) with text control/status events ('C' frames).
                Only 'C' frames are written to the log (as readable text) --
                raw audio bytes are counted but not persisted, since logging
                the actual audio stream to disk would grow unbounded during
                a long open-ended test session and isn't what's useful for
                debugging. Buffered chunk reads (not per-byte), matching the
                already-proven-correct approach from this project's earlier
                tap_classic_uart2.py (per-byte reads lost sync constantly at
                this baud rate).

Usage:
  serial_logger.py --by-id <serial> --baud <N> --mode line|framed --out <path>
"""
import argparse
import datetime
import os
import sys
import time

import serial

FRAME_MAGIC = 0xAA


def ts():
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]


def open_port(by_id_path, baud):
    while True:
        try:
            return serial.Serial(by_id_path, baud, timeout=0.2)
        except Exception as e:
            print(f"[logger] open failed ({e}), retrying in 2s...", file=sys.stderr)
            time.sleep(2)


def run_line_mode(by_id_path, baud, out_path):
    out = open(out_path, "a", buffering=1)
    out.write(f"\n=== logger (re)started {ts()} ===\n")
    ser = open_port(by_id_path, baud)
    out.write(f"=== port opened {ts()} ===\n")
    buf = b""
    while True:
        try:
            chunk = ser.read(4096)
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", errors="replace").rstrip("\r")
                out.write(f"[{ts()}] {text}\n")
        except (serial.SerialException, OSError) as e:
            out.write(f"=== port lost ({e}) at {ts()}, reconnecting... ===\n")
            try:
                ser.close()
            except Exception:
                pass
            ser = open_port(by_id_path, baud)
            out.write(f"=== port reopened {ts()} ===\n")
            buf = b""


def run_framed_mode(by_id_path, baud, out_path):
    out = open(out_path, "a", buffering=1)
    out.write(f"\n=== logger (re)started {ts()} ===\n")
    ser = open_port(by_id_path, baud)
    out.write(f"=== port opened {ts()} ===\n")
    buf = bytearray()
    n_audio_frames = 0
    n_audio_bytes = 0
    n_control_frames = 0
    n_resyncs = 0
    raw_text = bytearray()
    last_summary = time.time()

    def maybe_summary():
        nonlocal last_summary
        now = time.time()
        if now - last_summary >= 60:
            out.write(
                f"[{ts()}] --- summary: {n_audio_frames} audio frames, "
                f"{n_audio_bytes} audio bytes, {n_control_frames} control "
                f"frames, {n_resyncs} resyncs ---\n"
            )
            last_summary = now

    while True:
        try:
            chunk = ser.read(8192)
            if chunk:
                buf.extend(chunk)
            while True:
                if len(buf) < 6:
                    break
                if buf[0] != FRAME_MAGIC:
                    idx = buf.find(bytes([FRAME_MAGIC]), 1)
                    n_resyncs += 1
                    skipped = bytes(buf) if idx == -1 else bytes(buf[:idx])
                    if idx == -1:
                        buf.clear()
                    else:
                        del buf[:idx]
                    # Unframed bytes on this wire are raw ESP-IDF/Bluedroid
                    # log output (ESP_LOGE etc.), not audio -- keep any
                    # readable text instead of silently discarding it.
                    raw_text.extend(skipped)
                    while b"\n" in raw_text:
                        line, _, rest = bytes(raw_text).partition(b"\n")
                        raw_text[:] = rest
                        text = line.decode("ascii", errors="ignore").strip()
                        if len(text) >= 8 and sum(c.isprintable() for c in text) >= 0.9 * len(text):
                            out.write(f"[{ts()}] [raw] {text}\n")
                    if len(raw_text) > 4096:
                        raw_text.clear()
                    continue
                ftype = chr(buf[1])
                length = (buf[2] << 24) | (buf[3] << 16) | (buf[4] << 8) | buf[5]
                if length > 200000:
                    del buf[0]
                    n_resyncs += 1
                    continue
                if len(buf) < 6 + length:
                    break
                payload = bytes(buf[6:6 + length])
                del buf[:6 + length]
                if ftype in ("A", "P"):  # MP3, or raw PCM with ENCODE_ON_S3
                    n_audio_frames += 1
                    n_audio_bytes += length
                elif ftype == "C":
                    n_control_frames += 1
                    out.write(f"[{ts()}] {payload.decode(errors='replace')}\n")
            maybe_summary()
        except (serial.SerialException, OSError) as e:
            out.write(f"=== port lost ({e}) at {ts()}, reconnecting... ===\n")
            try:
                ser.close()
            except Exception:
                pass
            ser = open_port(by_id_path, baud)
            out.write(f"=== port reopened {ts()} ===\n")
            buf.clear()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--by-id", required=True)
    ap.add_argument("--baud", type=int, required=True)
    ap.add_argument("--mode", choices=["line", "framed"], required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)

    if args.mode == "line":
        run_line_mode(args.by_id, args.baud, args.out)
    else:
        run_framed_mode(args.by_id, args.baud, args.out)


if __name__ == "__main__":
    main()
