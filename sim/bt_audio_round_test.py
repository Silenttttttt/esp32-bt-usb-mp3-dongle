#!/usr/bin/python3
"""Two-device rounds WITH real audio, no un-pairing or resets anywhere:
laptop plays a tone into Golzin -> laptop Bluetooth off -> desktop connects ->
desktop Bluetooth off -> laptop reconnects and plays again. Checks the audio
actually arrives at the classic (AUDIO_LIVE and PCM_PEAK in its log) and that
the classic never reboots. Uses the helpers in sim/bt_switch_test.py.

  sim/bt_audio_round_test.py [rounds]    default 3
"""
import re
import sys
import time

from bt_switch_test import CLASSIC_LOG, Desktop, Laptop, classic_resets, ssh

TONE_S = 12


def log_since(t0_wall):
    stamp = time.strftime("[%Y-%m-%d %H:%M:%S", time.localtime(t0_wall))
    with open(CLASSIC_LOG, "rb") as f:
        f.seek(0, 2)
        f.seek(max(0, f.tell() - 3_000_000))
        lines = f.read().decode(errors="ignore").splitlines()
    return [l for l in lines if l[:20] >= stamp]


def play_tone_and_check(laptop):
    t0 = time.time()
    # The laptop's Bluetooth output starts at 0% volume (found 2026-09-25: the
    # first run sent pure silence); set it so the tone is audible.
    ssh("XDG_RUNTIME_DIR=/run/user/1000 pactl set-sink-volume bluez_output.GOLZIN_MAC.1 80%")
    ssh("XDG_RUNTIME_DIR=/run/user/1000 timeout 30 ffmpeg -loglevel error -re -f lavfi "
        f"-i sine=frequency=440:duration={TONE_S} -ac 2 -f pulse "
        "-device bluez_output.GOLZIN_MAC.1 golzin_test", TONE_S + 25)
    lines = log_since(t0)
    peaks = [int(m.group(1)) for l in lines for m in [re.search(r"PCM_PEAK:(\d+)", l)] if m]
    live = sum("|AUDIO_LIVE" in l for l in lines)
    return live, max(peaks) if peaks else 0


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    d, l = Desktop(), Laptop()
    resets0 = classic_resets()
    rows = []
    d.power(False)
    l.power(True)
    for r in range(1, rounds + 1):
        ok_l = l.connected() or any(l.connect() for _ in range(3))
        live, peak = play_tone_and_check(l) if ok_l else (0, 0)
        rows.append((r, "laptop connects + plays", "OK" if ok_l else "FAIL", f"AUDIO_LIVE x{live}, peak {peak}"))
        l.power(False)
        d.power(True)
        t0 = time.time()
        ok_d = any(d.connect() for _ in range(3))
        rows.append((r, "laptop BT off -> desktop connects", "OK" if ok_d else "FAIL", f"{time.time() - t0:.1f} s"))
        d.power(False)
        l.power(True)
        for row in rows[-2:]:
            print(" | ".join(str(x) for x in row), flush=True)
    rows.append(("-", "classic reboots during the test", str(classic_resets() - resets0), ""))
    d.power(True)
    print("\n| Round | Step | Result | Detail |\n|---|---|---|---|")
    for row in rows:
        print("| " + " | ".join(str(x) for x in row) + " |")


if __name__ == "__main__":
    main()
