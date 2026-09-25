#!/usr/bin/python3
"""Make this PC an A2DP audio SOURCE for Golzin (the classic ESP32) without
touching PipeWire/WirePlumber -- for two-device pairing/switch tests.

The desktop's audio server registers no Bluetooth audio endpoint, so BlueZ
refuses an A2DP connection ("a2dp-sink profile connect failed: Protocol not
available"). This registers a minimal SBC source endpoint (Media1 API) plus a
pairing agent that auto-accepts (confirm = yes, PIN = 0000), then runs until
killed. While it runs, `bluetoothctl connect <golzin-mac>` opens a real
A2DP connection; no audio is streamed (the connection is what's under test).

  sim/bt_desktop_source.py        run in the background, Ctrl-C / kill to stop
"""
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
SBC_CONFIG = dbus.Array([dbus.Byte(0x21), dbus.Byte(0x15), dbus.Byte(2), dbus.Byte(53)], signature="y")


class Endpoint(dbus.service.Object):
    @dbus.service.method("org.bluez.MediaEndpoint1", in_signature="ay", out_signature="ay")
    def SelectConfiguration(self, caps):
        print("endpoint: SelectConfiguration", list(caps), flush=True)
        return SBC_CONFIG

    @dbus.service.method("org.bluez.MediaEndpoint1", in_signature="oa{sv}", out_signature="")
    def SetConfiguration(self, transport, props):
        print("endpoint: SetConfiguration", transport, flush=True)

    @dbus.service.method("org.bluez.MediaEndpoint1", in_signature="o", out_signature="")
    def ClearConfiguration(self, transport):
        print("endpoint: ClearConfiguration", transport, flush=True)

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


def main():
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    Endpoint(bus, ENDPOINT_PATH)
    Agent(bus, AGENT_PATH)
    media = dbus.Interface(bus.get_object(BUS, ADAPTER), "org.bluez.Media1")
    media.RegisterEndpoint(ENDPOINT_PATH, {"UUID": A2DP_SOURCE_UUID, "Codec": dbus.Byte(SBC),
                                           "Capabilities": SBC_CAPS})
    mgr = dbus.Interface(bus.get_object(BUS, "/org/bluez"), "org.bluez.AgentManager1")
    mgr.RegisterAgent(AGENT_PATH, "DisplayYesNo")
    mgr.RequestDefaultAgent(AGENT_PATH)
    print("A2DP source endpoint + agent registered; running", flush=True)
    GLib.MainLoop().run()


if __name__ == "__main__":
    main()
