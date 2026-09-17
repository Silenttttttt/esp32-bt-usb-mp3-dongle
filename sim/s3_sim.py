#!/usr/bin/env python3
"""Synthetic stand-in for the real S3: receives the live MP3 byte stream
over a simulated wired UART link from the ESP32 (esp32_sim.py), appends it
into a real growing FAT12 volume, and serves that volume to the radio via
SCSI READ10-style sector reads -- the same interface a real S3 running
TinyUSB's USBMSC would expose. car_sim.py only ever talks to that one
READ10 boundary and doesn't know or care what's on the other end.
"""
import argparse
import socket
import sys
import threading
import time

from fat12_disk import GrowingFat12Disk
from sector_protocol import recv_read10_request, OPCODE_READ10, OPCODE_WATERMARK

disk_lock = threading.Lock()


def receive_from_esp32(conn, disk):
    total = 0
    start = time.monotonic()
    last_log = start
    try:
        while True:
            data = conn.recv(65536)
            if not data:
                break
            with disk_lock:
                disk.buffer.extend(data)
            total += len(data)
            now = time.monotonic()
            if now - last_log >= 1.0:
                print(f"[s3][uart-rx][timing] t={now - start:6.2f}s  received={total:8d}B "
                      f"from simulated UART", file=sys.stderr)
                last_log = now
    except ConnectionError:
        pass
    print(f"[s3] esp32 link closed, total received over UART: {total} bytes", file=sys.stderr)


def serve_radio(conn, disk):
    read_count = 0
    read_time_total = 0.0
    try:
        while True:
            opcode, lba, count = recv_read10_request(conn)
            if opcode == OPCODE_WATERMARK:
                with disk_lock:
                    watermark = len(disk.buffer)
                conn.sendall(watermark.to_bytes(8, "big"))
                continue
            if opcode != OPCODE_READ10:
                break
            t0 = time.monotonic()
            with disk_lock:
                data = disk.read_sectors(lba, count)
            read_time_total += time.monotonic() - t0
            read_count += 1
            conn.sendall(data)
    except ConnectionError:
        print("[s3] radio disconnected", file=sys.stderr)
    if read_count:
        print(f"[s3] served {read_count} READ10 requests, "
              f"avg handler latency {read_time_total / read_count * 1000:.3f}ms", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--esp32-port", type=int, default=9002, help="port the ESP32 (simulated UART) connects to")
    ap.add_argument("--radio-port", type=int, default=9003, help="port the radio connects to")
    ap.add_argument("--capacity-mb", type=float, default=8.0)
    args = ap.parse_args()

    disk = GrowingFat12Disk(capacity_bytes=int(args.capacity_mb * 1024 * 1024))
    print(f"[s3] FAT12 volume: {disk.total_sectors} sectors, "
          f"declared file size {disk.declared_file_size} bytes, "
          f"first data LBA {disk.first_data_lba}", file=sys.stderr)

    esp32_srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    esp32_srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    esp32_srv.bind(("127.0.0.1", args.esp32_port))
    esp32_srv.listen(1)
    print(f"[s3] waiting for esp32 (simulated UART) on port {args.esp32_port}...", file=sys.stderr)

    radio_srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    radio_srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    radio_srv.bind(("127.0.0.1", args.radio_port))
    radio_srv.listen(1)
    print(f"[s3] waiting for radio on port {args.radio_port}...", file=sys.stderr)

    esp32_conn, _ = esp32_srv.accept()
    print("[s3] esp32 connected", file=sys.stderr)
    threading.Thread(target=receive_from_esp32, args=(esp32_conn, disk), daemon=True).start()

    radio_conn, _ = radio_srv.accept()
    print("[s3] radio connected", file=sys.stderr)
    serve_radio(radio_conn, disk)


if __name__ == "__main__":
    main()
