# FAT16 fallback — NOT the primary firmware

**Use the primary firmware (`../esp32-s3-msc/`) first — it's confirmed working end-to-end on
real hardware, including a real car radio.** Only reach for this if the real car radio confirms
it won't mount the primary's FAT12 volume.

## Why this exists

Research on 2026-09-17 found a real, unconfirmed risk: many cheap embedded USB-MSC host stacks
(exactly the class of thing in a cheap aftermarket car radio) only fully support FAT16/FAT32
and may reject FAT12 entirely. The primary firmware uses FAT12 specifically because it's
optimized for a small declared volume (~470KB, ~30s of audio) — the whole point being to bound
the worst-case "how far behind live can the file's start be" lag. FAT16 requires at least 4085
data clusters, which forces a bigger minimum volume regardless of cluster size choice.

## What's different from the primary firmware

Only the filesystem type and its resulting size. Everything else — the ring buffer, the
`unread_protect` write backpressure, the straddle-avoidance read logic, the UART wire protocol
to the classic ESP32, the USBMSC wiring — is identical.

- **1-sector (512B) clusters**, not the primary's 8-sector (4096B) clusters. This specifically
  minimizes the size penalty of FAT16's 4085-cluster floor: 4096 clusters × 512B = 2MB, vs.
  ~16.7MB if using the primary's cluster size.
- **Declared volume: 2,097,152 bytes** (~131s / ~2.2 minutes of audio at 16000 B/s), vs. the
  primary's current ~4 minutes (raised from an original ~25.6s after real car-radio testing
  found an audible stutter at the ring's wrap point — see the primary firmware's own comments
  in `fat_disk_shared.h`). This is a real, meaningful UX tradeoff either way — a much bigger
  worst-case catch-up-lag bound — not a free fix. Don't treat switching to this as a minor
  detail if it's ever actually needed; it changes real behavior.

## What's been verified (no physical S3 board needed for any of this)

- Compiles clean (`arduino-cli compile --fqbn "esp32:esp32:esp32s3:USBMode=default,PSRAM=opi" .`
  from this directory).
- The FAT16 boot sector/FAT-table structure was mounted with **Linux's own real vfat kernel
  driver** via a loopback device (`../esp32-s3-msc/crosscheck/gen_fat16_image.cpp`) — confirmed
  to mount cleanly, present the file with the correct name/size, byte-exact content, and decode
  without error via a real, independent MP3 decoder (`mpg123`).
- The ring-buffer/backpressure/concurrency logic was re-verified specifically at this variant's
  much larger `DECLARED_FILE_SIZE` (2097152, vs. the primary's current 3842048) using the same
  real-time
  concurrent simulation methodology as the primary firmware — zero corruption across 1.14
  real-time ring laps. The logic is generic over ring size, not re-derived per filesystem type.

## What's NOT been verified

Everything hardware-dependent, same caveats as the primary firmware: never run on a real
ESP32-S3, UART pin wiring is a placeholder, real TinyUSB read-callback timing under real USB
host pressure is unconfirmed. And specific to this variant: it has never even been flashed —
only compiled. If this path is ever actually needed, flash and validate it with the same care
as the primary firmware got, not less just because "it's basically the same code."
