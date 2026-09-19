#!/usr/bin/env python3
"""Synthetic stand-in for the real ESP32: BT-sink + live MP3 encode, then
sends the encoded bytes out over a simulated wired UART link to the S3
(rate-limited to a configurable baud rate, exactly like a real serial TX
would be) -- standing in for the actual TX/RX + shared-power wiring
harness between the two boards. Talks to s3_sim.py over that one boundary
and doesn't know or care what's on the other end.
"""
import argparse
import socket
import subprocess
import sys
import threading
import time

SAMPLE_RATE = 44100
CHANNELS = 2
BYTES_PER_SAMPLE = 2
PCM_CHUNK_MS = 20
PCM_CHUNK_BYTES = int(SAMPLE_RATE * CHANNELS * BYTES_PER_SAMPLE * PCM_CHUNK_MS / 1000)

encode_lock = threading.Lock()
mp3_queue = bytearray()
encode_total_bytes = 0
encode_start_time = None
encode_done = threading.Event()


def audio_source_to_lame_stdin(source, duration, lame_stdin):
    """Stands in for: phone -> Bluetooth A2DP -> ESP32 PCM callback."""
    if source == "sine":
        cmd = [
            "ffmpeg", "-f", "lavfi", "-i", f"sine=frequency=440:duration={duration}",
            "-ar", str(SAMPLE_RATE), "-ac", str(CHANNELS), "-f", "s16le", "-",
        ]
    else:
        cmd = [
            "ffmpeg", "-i", source,
            "-ar", str(SAMPLE_RATE), "-ac", str(CHANNELS), "-f", "s16le", "-",
        ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)

    chunk_interval = PCM_CHUNK_MS / 1000.0
    next_send = time.monotonic()
    total = 0
    while True:
        data = proc.stdout.read(PCM_CHUNK_BYTES)
        if not data:
            break
        lame_stdin.write(data)
        total += len(data)
        next_send += chunk_interval
        sleep_time = next_send - time.monotonic()
        if sleep_time > 0:
            time.sleep(sleep_time)
    lame_stdin.close()
    print(f"[esp32] audio source exhausted, fed {total} bytes of PCM "
          f"({total / (SAMPLE_RATE * CHANNELS * BYTES_PER_SAMPLE):.2f}s of audio)", file=sys.stderr)


def lame_stdout_to_queue(lame_stdout, bitrate_kbps):
    """Encoder output -> the queue waiting to go out over the simulated
    UART link. Measures raw encode throughput vs real time."""
    global encode_total_bytes, encode_start_time
    encode_start_time = time.monotonic()
    last_log = encode_start_time
    while True:
        data = lame_stdout.read1(4096)
        if not data:
            break
        with encode_lock:
            mp3_queue.extend(data)
            encode_total_bytes += len(data)

        now = time.monotonic()
        if now - last_log >= 1.0:
            elapsed = now - encode_start_time
            audio_seconds = (encode_total_bytes * 8) / (bitrate_kbps * 1000)
            print(f"[esp32][timing] t={elapsed:6.2f}s  mp3_bytes={encode_total_bytes:8d}  "
                  f"audio_encoded={audio_seconds:6.2f}s  "
                  f"encode_speed={(audio_seconds / elapsed if elapsed else 0):5.2f}x realtime  "
                  f"queued_for_uart={len(mp3_queue):6d}B", file=sys.stderr)
            last_log = now

    elapsed = time.monotonic() - encode_start_time
    audio_seconds = (encode_total_bytes * 8) / (bitrate_kbps * 1000)
    print(f"[esp32] encoding finished: {encode_total_bytes} bytes, {audio_seconds:.2f}s of audio "
          f"in {elapsed:.2f}s wall time (avg {audio_seconds / elapsed:.2f}x realtime)", file=sys.stderr)
    encode_done.set()


def queue_to_uart(sock, baud):
    """Sends the queued MP3 bytes to the S3 at exactly the throughput a
    real UART at this baud rate would allow (8N1 framing = 10 bits/byte),
    so we can tell whether the link itself -- not just the encoder -- can
    keep up in real time."""
    bytes_per_sec = baud / 10.0
    chunk = 64
    interval = chunk / bytes_per_sec
    sent_total = 0
    start = time.monotonic()
    last_log = start
    while True:
        with encode_lock:
            if len(mp3_queue) >= chunk:
                out = bytes(mp3_queue[:chunk])
                del mp3_queue[:chunk]
            elif mp3_queue:
                out = bytes(mp3_queue)
                mp3_queue.clear()
            else:
                out = b""
        if out:
            sock.sendall(out)
            sent_total += len(out)
            time.sleep(interval)
        else:
            if encode_done.is_set():
                break
            time.sleep(0.01)

        now = time.monotonic()
        if now - last_log >= 1.0:
            elapsed = now - start
            print(f"[esp32->uart][timing] t={elapsed:6.2f}s  sent={sent_total:8d}B  "
                  f"link_capacity={bytes_per_sec:8.0f}B/s  backlog={len(mp3_queue):6d}B", file=sys.stderr)
            last_log = now
    elapsed = time.monotonic() - start
    print(f"[esp32->uart] done, sent {sent_total} bytes total over {elapsed:.2f}s "
          f"(avg {sent_total / elapsed:.0f} B/s)", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", default="sine")
    ap.add_argument("--duration", type=int, default=20)
    ap.add_argument("--s3-port", type=int, default=9002, help="port to connect to on the S3 side")
    ap.add_argument("--bitrate", type=int, default=64, help="MP3 encode bitrate (kbps)")
    ap.add_argument("--quality", type=int, default=7, help="LAME -q speed/quality tradeoff, 0=best/slowest 9=fastest/worst")
    ap.add_argument("--baud", type=int, default=921600, help="simulated wired UART baud rate")
    args = ap.parse_args()

    print(f"[esp32] connecting to s3_sim on port {args.s3_port}...", file=sys.stderr)
    sock = socket.create_connection(("127.0.0.1", args.s3_port))
    print(f"[esp32] connected, simulated UART at {args.baud} baud "
          f"(~{args.baud / 10:.0f} B/s effective)", file=sys.stderr)

    lame = subprocess.Popen(
        ["stdbuf", "-o0",
         "lame", "-r", "-s", "44.1", "--bitwidth", "16", "--little-endian",
         "-b", str(args.bitrate), "-q", str(args.quality), "-", "-"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
    )

    threading.Thread(target=audio_source_to_lame_stdin, args=(args.source, args.duration, lame.stdin), daemon=True).start()
    threading.Thread(target=lame_stdout_to_queue, args=(lame.stdout, args.bitrate), daemon=True).start()
    queue_to_uart(sock, args.baud)


if __name__ == "__main__":
    main()
