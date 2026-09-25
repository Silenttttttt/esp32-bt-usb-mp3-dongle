#!/usr/bin/python3
"""Make this PC an A2DP audio SOURCE for Golzin (the classic ESP32) without
touching PipeWire/WirePlumber -- for two-device pairing/switch tests.

The desktop's audio server registers no Bluetooth audio endpoint, so BlueZ
refuses an A2DP connection ("a2dp-sink profile connect failed: Protocol not
available"). This registers a minimal SBC source endpoint (Media1 API) plus a
pairing agent that auto-accepts (confirm = yes, PIN = 0000), then runs until
killed. While it runs, `bluetoothctl connect <golzin-mac>` opens a real
A2DP connection. `kill -USR1 <pid>` then streams a 12 s 440 Hz tone over it
(ffmpeg's SBC encoder, packetized as A2DP RTP into the BlueZ transport).

  sim/bt_desktop_source.py        run in the background, Ctrl-C / kill to stop
"""
import os
import signal
import struct
import subprocess
import threading
import time

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

BUS = "org.bluez"
ADAPTER = "/org/bluez/hci0"
ENDPOINT_PATH = "/golzin/sbc_source"
AGENT_PATH = "/golzin/desktop_agent"
A2DP_SOURCE_UUID = "0000110a-0000-1000-8000-00805f9b34fb"
SBC = 0x00
# Every SBC option; bitpool 2..53. SelectConfiguration picks 44.1 kHz joint stereo.
SBC_CAPS = dbus.Array([dbus.Byte(0xFF), dbus.Byte(0xFF), dbus.Byte(2), dbus.Byte(53)], signature="y")
# 44.1 kHz stereo, 16 blocks, 8 subbands, loudness -- what ffmpeg's SBC encoder produces.
SBC_CONFIG = dbus.Array([dbus.Byte(0x22), dbus.Byte(0x15), dbus.Byte(2), dbus.Byte(53)], signature="y")
TONE_S = 12
state = {"transport": None, "bus": None}


class Endpoint(dbus.service.Object):
    @dbus.service.method("org.bluez.MediaEndpoint1", in_signature="ay", out_signature="ay")
    def SelectConfiguration(self, caps):
        print("endpoint: SelectConfiguration", list(caps), flush=True)
        return SBC_CONFIG

    @dbus.service.method("org.bluez.MediaEndpoint1", in_signature="oa{sv}", out_signature="")
    def SetConfiguration(self, transport, props):
        print("endpoint: SetConfiguration", transport, flush=True)
        state["transport"] = transport

    @dbus.service.method("org.bluez.MediaEndpoint1", in_signature="o", out_signature="")
    def ClearConfiguration(self, transport):
        print("endpoint: ClearConfiguration", transport, flush=True)
        if state["transport"] == transport:
            state["transport"] = None

    @dbus.service.method("org.bluez.MediaEndpoint1", in_signature="", out_signature="")
    def Release(self):
        pass


class Agent(dbus.service.Object):
    @dbus.service.method("org.bluez.Agent1", in_signature="", out_signature="")
    def Release(self):
        pass

    @dbus.service.method("org.bluez.Agent1", in_signature="os", out_signature="")
    def AuthorizeService(self, device, uuid):
        return

    @dbus.service.method("org.bluez.Agent1", in_signature="ou", out_signature="")
    def RequestConfirmation(self, device, passkey):
        print(f"agent: confirm passkey {passkey:06d}: yes", flush=True)

    @dbus.service.method("org.bluez.Agent1", in_signature="o", out_signature="")
    def RequestAuthorization(self, device):
        return

    @dbus.service.method("org.bluez.Agent1", in_signature="o", out_signature="s")
    def RequestPinCode(self, device):
        print("agent: PIN 0000", flush=True)
        return "0000"

    @dbus.service.method("org.bluez.Agent1", in_signature="", out_signature="")
    def Cancel(self):
        pass


def stream_tone(fd, mtu):
    """Send TONE_S seconds of a 440 Hz tone as A2DP SBC RTP packets, at real time."""
    sbc = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
                          f"sine=frequency=440:duration={TONE_S}", "-ar", "44100", "-ac", "2",
                          "-c:a", "sbc", "-b:a", "229k", "-f", "sbc", "-"], capture_output=True).stdout
    h = sbc[1]
    blocks, sub, bitpool = [4, 8, 12, 16][(h >> 4) & 3], [4, 8][h & 1], sbc[2]
    flen = 4 + (4 * sub * 2) // 8 + -(-(blocks * bitpool) // 8)  # stereo (mode 2)
    frames = [sbc[i:i + flen] for i in range(0, len(sbc) - flen + 1, flen)]
    per_pkt = max(1, min(15, (mtu - 13) // flen))
    samples = blocks * sub
    seq, ts, t0 = 0, 0, time.monotonic()
    for i in range(0, len(frames), per_pkt):
        chunk = frames[i:i + per_pkt]
        pkt = struct.pack(">BBHII", 0x80, 0x60, seq & 0xFFFF, ts & 0xFFFFFFFF, 1) + bytes([len(chunk)]) + b"".join(chunk)
        try:
            os.write(fd, pkt)
        except OSError as e:
            print("tone: write failed:", e, flush=True)
            break
        seq += 1
        ts += samples * len(chunk)
        delay = t0 + ts / 44100 - time.monotonic()
        if delay > 0:
            time.sleep(delay)
    print(f"tone: sent {seq} packets ({len(frames)} SBC frames of {flen} B)", flush=True)


def on_play():
    tp = state["transport"]
    if not tp:
        print("tone: no A2DP transport (connect Golzin first)", flush=True)
        return True
    tr = dbus.Interface(state["bus"].get_object(BUS, tp), "org.bluez.MediaTransport1")
    try:
        fd, mtu_r, mtu_w = tr.Acquire()
    except dbus.DBusException as e:
        print("tone: Acquire failed:", e, flush=True)
        return True
    fd = fd.take()
    print(f"tone: acquired {tp} (mtu {int(mtu_w)})", flush=True)

    def run():
        try:
            stream_tone(fd, int(mtu_w))
        finally:
            os.close(fd)
            GLib.idle_add(lambda: (tr.Release(), False)[1])
    threading.Thread(target=run, daemon=True).start()
    return True


def main():
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    state["bus"] = bus
    Endpoint(bus, ENDPOINT_PATH)
    Agent(bus, AGENT_PATH)
    media = dbus.Interface(bus.get_object(BUS, ADAPTER), "org.bluez.Media1")
    media.RegisterEndpoint(ENDPOINT_PATH, {"UUID": A2DP_SOURCE_UUID, "Codec": dbus.Byte(SBC),
                                           "Capabilities": SBC_CAPS})
    mgr = dbus.Interface(bus.get_object(BUS, "/org/bluez"), "org.bluez.AgentManager1")
    mgr.RegisterAgent(AGENT_PATH, "DisplayYesNo")
    mgr.RequestDefaultAgent(AGENT_PATH)
    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGUSR1, on_play)
    print("A2DP source endpoint + agent registered; running (SIGUSR1 = play a tone)", flush=True)
    GLib.MainLoop().run()


if __name__ == "__main__":
    main()
