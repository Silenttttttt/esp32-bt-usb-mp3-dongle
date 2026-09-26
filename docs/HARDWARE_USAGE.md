# Hardware usage (v2, the car build)

Measured 2026-09-25 on the bench:
- Firmware: classic at `9c03c6c`, S3 at `ce23a07` (the maximum ring, 1360 clusters per file), flags from
  [BUILD_FLAGS.md](../BUILD_FLAGS.md).
- Load: steady streaming, both with the phone connected and with a laptop playing a tone.
- Build-time numbers are from the compiler. Runtime numbers are from the boards' own heartbeat
  lines (`FRAG:` on the classic, `[s3] mem:` / `[s3] enc:` on the S3), over 1-3 minutes each.

## What each board has

| | Classic ESP32 (ESP32-WROOM-32) | ESP32-S3 (DevKitC-1, `PSRAM=opi`) |
|---|---|---|
| CPU | 2 × Xtensa LX6, 240 MHz | 2 × Xtensa LX7, 240 MHz |
| Internal RAM | 520 KB SRAM; the Arduino app sees 320 KB of DRAM | 512 KB SRAM; the Arduino app sees 320 KB of DRAM |
| PSRAM | none | 8 MB (octal) |
| App flash (partition) | 1,310,720 B (1.25 MB, default partition table) | 1,310,720 B (1.25 MB) |
| Radios / USB | Bluetooth Classic (A2DP sink, AVRCP) | Native USB (full speed, 12 Mbit/s) as a mass-storage device |
| Serial links | UART0 TX -> S3 (the audio link); UART2 RX <- S3 (button relay) | UART1 RX <- classic; UART2 TX -> classic |

## Flash and static RAM (compiler)

| | Used | Of | % |
|---|---|---|---|
| Classic app flash | 1,084,884 B | 1,310,720 B | **83%** |
| Classic static RAM (globals) | 56,568 B | 327,680 B | 17% |
| S3 app flash | 494,228 B | 1,310,720 B | 38% |
| S3 static RAM (globals) | 81,080 B | 327,680 B | 25% |

The classic's flash is the tightest resource: ~220 KB left. The Bluetooth stack (Bluedroid, A2DP,
AVRCP) is most of it.

## RAM at runtime

| | Total | Free (typical) | Used | Lowest free seen | Largest free block |
|---|---|---|---|---|---|
| Classic heap | 254,832 B | ~100,000 B (39%) | ~155,000 B (**61%**) | 70,468 B (during a laptop tone) | ~78-82 KB |
| S3 internal heap | 327,336 B | 162,320 B (50%) | 165,016 B (**50%**) | 162,128 B | 120,820 B |
| S3 PSRAM | 8,388,608 B | 2,727,920 B (33%) | 5,660,688 B (**67%**) | constant | 2.69 MB |

What uses it:

| Item | Where | Size | Note |
|---|---|---|---|
| Audio ring (the "files") | S3 PSRAM | 5,570,560 B (1360 clusters × 4 KB, ~348 s at 128 kbps) | Almost all of the PSRAM use. 3 files alias it. The maximum FAT12 allows with 3 files; a single-file (v1) build uses 8,192,000 B |
| Shine MP3 encoder | S3 internal heap (allocations under 256 KB stay internal; `heap_caps_malloc_extmem_enable(256 KB)`) | ~80 KB | On the classic it starved AVRCP; on the S3 it fits comfortably |
| UART receive buffer | S3 internal heap | 16 KB | ~0.18 s of PCM at 88 KB/s |
| FAT metadata (boot sector, FAT, 64-entry root dir) | S3 static | ~8.5 KB | |
| Bluetooth stack (Bluedroid + A2DP + AVRCP) | Classic heap | most of the ~155 KB in use (not measured separately) | Why the encoder had to move off the classic |
| PCM slots | Classic static | 3 × 4 KB | The A2DP callback -> loop() handoff |

## CPU and links

| | Busy | Capacity | % | Note |
|---|---|---|---|---|
| S3 MP3 encoding (per 23.2 ms chunk of mono 44.1 kHz) | 4.3 ms avg on silence, 5.2-5.9 ms avg on audio, 10.2 ms worst | 23.2 ms | **18-25% of one core** avg, 44% worst | Link task, pinned to core 1 |
| Classic, sending each PCM chunk | ~10.4 ms per 23.2 ms chunk | 23.2 ms | 45% of loop time | Almost all waiting on the 2 Mbaud wire, not computing |
| Classic -> S3 link | ~89 KB/s (88.2 KB/s PCM + framing) ≈ 0.89 Mbit/s | 2 Mbit/s | **45%** | S3 RX FIFO threshold 32 B (0 overflows since the fix) |
| S3 -> classic return link | a few lines per second (heartbeat, button relay) | 9600 baud | ~1% | Slow on purpose (noise margin) |
| USB, radio reads | ~16 KB/s, plus ~100 KB bursts at a file open | 12 Mbit/s full speed | ~1% | 32 KB reads take ~33 ms |
| Bluetooth A2DP from the phone | ~230-330 kbit/s SBC | - | - | Decoded by the classic's Bluetooth stack |
