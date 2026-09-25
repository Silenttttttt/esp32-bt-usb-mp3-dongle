#!/usr/bin/python3
"""Pair and connect this machine to the classic ESP32 ("Golzin") over BlueZ D-Bus,
without bluetoothctl (not installed on the laptop). Registers a NoInputNoOutput
agent for the pairing, scans for the name, then Pair + Trust + Connect.

  sim/bt_pair_laptop.py [name]        pair + connect (default name: Golzin)
  sim/bt_pair_laptop.py --disconnect  disconnect it
  sim/bt_pair_laptop.py --remove      forget it (unpair)
Car session 2026-09-25: "a second device pairs" test (the classic used to accept
only one device ever).
"""
import sys
import time

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

BUS = "org.bluez"
AGENT_PATH = "/golzin/agent"


class Agent(dbus.service.Object):
    @dbus.service.method("org.bluez.Agent1", in_signature="", out_signature="")
    def Release(self):
        pass

    @dbus.service.method("org.bluez.Agent1", in_signature="os", out_signature="")
    def AuthorizeService(self, device, uuid):
        return

    @dbus.service.method("org.bluez.Agent1", in_signature="ou", out_signature="")
    def RequestConfirmation(self, device, passkey):
        print(f"confirm passkey {passkey:06d}: yes")

    @dbus.service.method("org.bluez.Agent1", in_signature="o", out_signature="")
    def RequestAuthorization(self, device):
        return

    @dbus.service.method("org.bluez.Agent1", in_signature="o", out_signature="s")
    def RequestPinCode(self, device):
        return "0000"

    @dbus.service.method("org.bluez.Agent1", in_signature="", out_signature="")
    def Cancel(self):
        pass


def find(bus, name):
    om = dbus.Interface(bus.get_object(BUS, "/"), "org.freedesktop.DBus.ObjectManager")
    for path, ifaces in om.GetManagedObjects().items():
        d = ifaces.get("org.bluez.Device1")
        if d and (d.get("Name") == name or d.get("Alias") == name):
            return path, d
    return None, None


def main():
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    name = args[0] if args else "Golzin"
    path, props = find(bus, name)

    if "--disconnect" in sys.argv or "--remove" in sys.argv:
        if not path:
            print(f"{name}: not known")
            return 1
        dev = dbus.Interface(bus.get_object(BUS, path), "org.bluez.Device1")
        if "--remove" in sys.argv:
            adapter = dbus.Interface(bus.get_object(BUS, "/org/bluez/hci0"), "org.bluez.Adapter1")
            adapter.RemoveDevice(path)
            print(f"{name}: removed")
        else:
            dev.Disconnect()
            print(f"{name}: disconnected")
        return 0

    Agent(bus, AGENT_PATH)
    mgr = dbus.Interface(bus.get_object(BUS, "/org/bluez"), "org.bluez.AgentManager1")
    mgr.RegisterAgent(AGENT_PATH, "NoInputNoOutput")
    mgr.RequestDefaultAgent(AGENT_PATH)

    adapter = dbus.Interface(bus.get_object(BUS, "/org/bluez/hci0"), "org.bluez.Adapter1")
    if not path:
        print(f"scanning for {name}...")
        adapter.StartDiscovery()
        deadline = time.time() + 30
        ctx = GLib.MainContext.default()
        while time.time() < deadline and not path:
            while ctx.pending():
                ctx.iteration(False)
            time.sleep(0.5)
            path, props = find(bus, name)
        try:
            adapter.StopDiscovery()
        except dbus.DBusException:
            pass
        if not path:
            print(f"{name}: not found in 30 s (is it discoverable / not connected to the phone?)")
            return 1
    print(f"{name}: {path} ({props.get('Address')})")
    dev_obj = bus.get_object(BUS, path)
    dev = dbus.Interface(dev_obj, "org.bluez.Device1")
    pr = dbus.Interface(dev_obj, "org.freedesktop.DBus.Properties")

    # Async + main loop: during Pair, BlueZ calls back into our agent
    # (RequestConfirmation), which a blocking call would deadlock.
    def call(fn, what):
        t0 = time.time()
        loop = GLib.MainLoop()
        result = {}

        def ok(*_):
            result["ok"] = True
            loop.quit()

        def err(e):
            result["err"] = e
            loop.quit()

        fn(reply_handler=ok, error_handler=err, timeout=40)
        loop.run()
        if result.get("ok"):
            print(f"{what}: ok ({time.time() - t0:.1f}s)")
            return True
        e = result["err"]
        print(f"{what}: FAILED ({time.time() - t0:.1f}s): {e.get_dbus_name()}: {e.get_dbus_message()}")
        return False

    if not pr.Get("org.bluez.Device1", "Paired"):
        if not call(dev.Pair, "pair"):
            return 1
    pr.Set("org.bluez.Device1", "Trusted", dbus.Boolean(True))
    return 0 if call(dev.Connect, "connect") else 1


if __name__ == "__main__":
    sys.exit(main())
