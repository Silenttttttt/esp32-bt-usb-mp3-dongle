#!/usr/bin/python3
"""Two-device switch test for Golzin (the classic ESP32): the desktop and the
laptop take turns as "the phone". Each switch: the connected device goes away
(Bluetooth powered off = phone BT off / out of range, or a clean disconnect),
then the other device keeps trying to connect until it gets an A2DP link.

Needs sim/bt_desktop_source.py running on the desktop (A2DP source endpoint
+ agent) and the laptop reachable over SSH with sim/bt_pair_laptop.py.
Configuration (nothing personal lives in this file):
  GOLZIN_TEST_REMOTE   ssh target for the second machine, e.g. user@laptop-host (required)
  GOLZIN_MAC           the classic's Bluetooth address (default: looked up by name "Golzin")
  SSH password: sim/.sudo_password (git-ignored), or leave it empty for key auth.

  sim/bt_switch_test.py [cycles]      default 3 cycles of each variant
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def _golzin_mac():
    mac = os.environ.get("GOLZIN_MAC", "")
    if mac:
        return mac
    out = subprocess.run(["bluetoothctl", "devices"], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[2].strip() == "Golzin":
            return parts[1]
    sys.exit("Golzin not known to this PC's BlueZ: pair it once, or set GOLZIN_MAC")


GOLZIN = _golzin_mac()
LAPTOP = os.environ.get("GOLZIN_TEST_REMOTE") or sys.exit("set GOLZIN_TEST_REMOTE=user@host (the second machine)")
DEV_PATH = "/org/bluez/hci0/dev_" + GOLZIN.replace(":", "_")
SINK = "bluez_output." + GOLZIN.replace(":", "_") + ".1"
CLASSIC_LOG = os.path.join(HERE, "..", "logs", "classic_serial.log")
_pw_file = os.path.join(HERE, ".sudo_password")
PASSWORD = open(_pw_file).read().strip() if os.path.exists(_pw_file) else ""


def sh(cmd, timeout=60):
    try:
        return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout).stdout
    except subprocess.TimeoutExpired:
        return ""


def ssh(cmd, timeout=60):
    quoted = subprocess.list2cmdline([cmd])
    if PASSWORD:
        env = dict(os.environ, SSHPASS=PASSWORD)
        full = f"sshpass -e ssh -o PubkeyAuthentication=no -o ConnectTimeout=8 {LAPTOP} {quoted}"
    else:
        env = None
        full = f"ssh -o ConnectTimeout=8 {LAPTOP} {quoted}"
    try:
        return subprocess.run(full, shell=True, capture_output=True, text=True, timeout=timeout, env=env).stdout
    except subprocess.TimeoutExpired:
        return ""


def btctl(*cmds, wait=1):
    script = "; ".join([f"echo '{c}'; sleep {wait}" for c in cmds]) + "; echo quit"
    return sh(f"({script}) | bluetoothctl", timeout=wait * len(cmds) + 20)


class Desktop:
    name = "desktop"

    def power(self, on):
        btctl(f"power {'on' if on else 'off'}", wait=2)

    def connect(self):
        out = btctl(f"connect {GOLZIN}", wait=12)
        return "Connection successful" in out

    def disconnect(self):
        btctl(f"disconnect {GOLZIN}", wait=3)

    def connected(self):
        return "Connected: yes" in sh(f"bluetoothctl info {GOLZIN}")


class Laptop:
    name = "laptop"

    def power(self, on):
        ssh(f"busctl --system set-property org.bluez /org/bluez/hci0 org.bluez.Adapter1 Powered b {'true' if on else 'false'}")
        time.sleep(1)

    def connect(self):
        out = ssh("cd ~/Documents/Computarias/digispark-msc && timeout 50 /usr/bin/python3 sim/bt_pair_laptop.py", 70)
        return "connect: ok" in out

    def disconnect(self):
        ssh("cd ~/Documents/Computarias/digispark-msc && /usr/bin/python3 sim/bt_pair_laptop.py --disconnect")

    def connected(self):
        return "true" in ssh(f"busctl --system get-property org.bluez {DEV_PATH} org.bluez.Device1 Connected")


def classic_resets():
    try:
        with open(CLASSIC_LOG, "rb") as f:
            f.seek(0, 2)
            f.seek(max(0, f.tell() - 5_000_000))
            data = f.read().decode(errors="ignore")
    except OSError:
        return 0
    return data.count("RESET_REASON:")


def ensure_connected(dev, other):
    if dev.connected():
        return True
    other.disconnect()
    for _ in range(4):
        if dev.connect():
            return True
        time.sleep(3)
    return dev.connected()


def switch(src, dst, variant, limit_s=90):
    """src is connected; it goes away (variant); dst tries until connected."""
    if variant == "power_off":
        src.power(False)
    else:
        src.disconnect()
    t0 = time.time()
    attempts = 0
    ok = False
    while time.time() - t0 < limit_s:
        attempts += 1
        if dst.connect():
            ok = True
            break
        time.sleep(3)
    dt = time.time() - t0
    if variant == "power_off":
        src.power(True)
    return ok, dt, attempts


def main():
    cycles = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    d, l = Desktop(), Laptop()
    d.power(True)
    l.power(True)
    resets0 = classic_resets()
    rows = []
    if not ensure_connected(d, l):
        print("could not get the desktop connected to start with")
        return 1
    src, dst = d, l
    for variant in ("power_off", "disconnect"):
        for c in range(cycles * 2):
            ok, dt, attempts = switch(src, dst, variant)
            row = (variant, f"{src.name} -> {dst.name}", "OK" if ok else "FAIL", f"{dt:.1f}", attempts,
                   classic_resets() - resets0)
            rows.append(row)
            print(" | ".join(str(x) for x in row), flush=True)
            if ok:
                src, dst = dst, src
            else:
                ensure_connected(src, dst)
            time.sleep(3)
    print("\n| Variant | Switch | Result | Seconds | Attempts | Classic resets so far |")
    print("|---|---|---|---|---|---|")
    for r in rows:
        print("| " + " | ".join(str(x) for x in r) + " |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
