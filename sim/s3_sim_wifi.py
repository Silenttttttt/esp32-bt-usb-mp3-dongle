#!/usr/bin/env python3
"""Synthetic stand-in for the real S3, now talking to the REAL ESP32 over
WiFi (standing in for the eventual wired UART link -- the real ESP32 only
has USB-serial available during this bench test, so WiFi is the practical
substitute). Same disk-serving role as s3_sim.py: receives the live MP3
stream, appends it into a real growing FAT12 volume, and serves it to the
radio via SCSI READ10-style sector reads.

New here: parses the ESP32's framed protocol (1-byte type + 4-byte
big-endian length + payload) so real AVRCP play/pause/track-change events
from a real phone show up as real, timestamped log lines -- this is where
actual measured delays (phone action -> event received here) come from.
"""
import argparse
import socket
import struct
import sys
import threading
import time

from fat12_disk import GrowingFat12Disk
from sector_protocol import recv_read10_request, OPCODE_READ10, OPCODE_WATERMARK

disk_lock = threading.Lock()


def recv_exact(sock, n):
    data = b""
    while len(data) < n:
        more = sock.recv(n - len(data))
        if not more:
            raise ConnectionError("connection closed mid-read")
        data += more
    return data


def receive_from_esp32(conn, disk):
    total = 0
    control_count = 0
    start = time.monotonic()
    last_log = start
    try:
        while True:
            header = recv_exact(conn, 5)
            frame_type = chr(header[0])
            length = struct.unpack(">I", header[1:5])[0]
            payload = recv_exact(conn, length) if length else b""

            if frame_type == "A":
                with disk_lock:
                    disk.buffer.extend(payload)
                total += len(payload)
            elif frame_type == "C":
                control_count += 1
                now = time.monotonic()
                text = payload.decode("utf-8", errors="replace")
                # ESP32 side format: "<esp32_millis>|<event>"
                esp_ms, _, event = text.partition("|")
                print(f"[s3][CONTROL] t={now - start:7.3f}s  esp32_ms={esp_ms:>10}  event={event}",
                      file=sys.stderr)
            else:
                print(f"[s3] unknown frame type {header[0]!r}, {length} bytes discarded", file=sys.stderr)

            now = time.monotonic()
            if now - last_log >= 1.0:
                print(f"[s3][wifi-rx][timing] t={now - start:6.2f}s  audio_received={total:8d}B  "
                      f"control_events={control_count}", file=sys.stderr)
                last_log = now
    except ConnectionError:
        pass
    print(f"[s3] esp32 link closed, total audio received: {total} bytes, "
          f"{control_count} control events", file=sys.stderr)


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
    ap.add_argument("--esp32-port", type=int, default=9400, help="port the real ESP32 connects to over WiFi/LAN")
    ap.add_argument("--radio-port", type=int, default=9401, help="port the radio connects to")
    ap.add_argument("--capacity-mb", type=float, default=8.0)
    args = ap.parse_args()

    disk = GrowingFat12Disk(capacity_bytes=int(args.capacity_mb * 1024 * 1024))
    print(f"[s3] FAT12 volume: {disk.total_sectors} sectors, "
          f"declared file size {disk.declared_file_size} bytes, "
          f"first data LBA {disk.first_data_lba}", file=sys.stderr)

    esp32_srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    esp32_srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    esp32_srv.bind(("0.0.0.0", args.esp32_port))  # listen on the LAN, not just localhost
    esp32_srv.listen(1)
    print(f"[s3] waiting for the real ESP32 on 0.0.0.0:{args.esp32_port}...", file=sys.stderr)

    radio_srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    radio_srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    radio_srv.bind(("127.0.0.1", args.radio_port))
    radio_srv.listen(1)
    print(f"[s3] waiting for radio on port {args.radio_port}...", file=sys.stderr)

    def esp32_accept_loop():
        while True:
            esp32_conn, esp32_addr = esp32_srv.accept()
            print(f"[s3] ESP32 connected from {esp32_addr}", file=sys.stderr)
            # TCP keepalive: detects a peer that vanished without a clean
            # close (e.g. a hard hardware reset) within seconds, so a dead
            # connection can't block the accept loop forever -- doesn't
            # interfere with a genuinely idle-but-alive link (a paused
            # stream), since it operates below the application layer.
            esp32_conn.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
            esp32_conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPIDLE, 5)
            esp32_conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPINTVL, 2)
            esp32_conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPCNT, 3)
            receive_from_esp32(esp32_conn, disk)  # blocks until this connection drops, then loop to accept again

    def radio_accept_loop():
        while True:
            radio_conn, _ = radio_srv.accept()
            print("[s3] radio connected", file=sys.stderr)
            serve_radio(radio_conn, disk)  # blocks until this connection drops, then loop to accept again

    threading.Thread(target=esp32_accept_loop, daemon=True).start()
    radio_accept_loop()  # run the radio-accept loop on the main thread


if __name__ == "__main__":
    main()
