#!/usr/bin/env python3
"""Rigorous T3 measurement: the exact wall-clock instant real, audible sound
starts coming out of the speaker -- measured directly from actual output
audio amplitude, not inferred/assumed from documentation history or any
other layer's timing.

Captures raw PCM from the real audio sink's monitor via parec (so this is
literally the same signal the physical speaker receives, not a proxy),
computes short-window RMS energy, and logs a wall-clock (epoch, same clock
as sim/s3_real_firmware_host.cpp's T1/T2 DELAY-MEASURE lines -- system_clock
in C++, time.time() here, both real epoch time, directly comparable with no
cross-process correlation needed) timestamp on every quiet-to-loud
transition. Run this ALONGSIDE the pipeline; after a test, the first
transition after T1 in the combined logs is T3.
"""
import argparse
import struct
import subprocess
import sys
import time

SAMPLE_RATE = 44100
CHANNELS = 2
CHUNK_SAMPLES = 2205  # 0.05s windows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sink-monitor", required=True,
                     help="exact PulseAudio/PipeWire monitor source name, e.g. "
                          "'alsa_output.XXXX.monitor' from `pactl list sources short`")
    ap.add_argument("--threshold", type=float, default=400.0,
                     help="RMS threshold (16-bit sample scale, 0-32767) above which audio "
                          "counts as 'real sound' rather than silence/noise floor. Tune by "
                          "watching the [level] lines this script prints and picking a value "
                          "clearly above the observed silence-floor RMS and clearly below "
                          "real-audio RMS.")
    args = ap.parse_args()

    proc = subprocess.Popen(
        ["parec", "--format=s16le", f"--rate={SAMPLE_RATE}", f"--channels={CHANNELS}",
         "-d", args.sink_monitor],
        stdout=subprocess.PIPE, bufsize=0,
    )
    chunk_bytes = CHUNK_SAMPLES * CHANNELS * 2
    was_loud = False
    print(f"[audio-monitor] capturing from {args.sink_monitor}, threshold={args.threshold}",
          file=sys.stderr)
    try:
        while True:
            buf = proc.stdout.read(chunk_bytes)
            if not buf:
                print("[audio-monitor] parec stream ended", file=sys.stderr)
                break
            n = len(buf) // 2
            if n == 0:
                continue
            samples = struct.unpack(f"<{n}h", buf[:n * 2])
            rms = (sum(s * s for s in samples) / n) ** 0.5
            now = time.time()
            is_loud = rms > args.threshold
            print(f"[audio-monitor][level] epoch={now:.6f} rms={rms:.1f}", file=sys.stderr)
            if is_loud and not was_loud:
                print(f"[audio-monitor][TRANSITION] quiet->loud epoch={now:.6f} rms={rms:.1f}",
                      file=sys.stderr, flush=True)
            elif was_loud and not is_loud:
                print(f"[audio-monitor][TRANSITION] loud->quiet epoch={now:.6f} rms={rms:.1f}",
                      file=sys.stderr, flush=True)
            was_loud = is_loud
    except KeyboardInterrupt:
        pass
    finally:
        proc.terminate()


if __name__ == "__main__":
    main()
