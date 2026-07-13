"""Pick the upload/monitor port by USB VID:PID -- or refuse to run.

Why this exists, concretely: CanTick (Espressif VID 0x303a) is a USB-CDC device
that lives on the same bench as Light (Seeed VID 0x2886). Both land on
/dev/cu.usbmodemNNNN (or /dev/ttyACMn), and the index is assigned by enumeration
order, not by which board it is. A pinned port is one replug away from pointing
the flasher at the wrong device.

Leaving `upload_port` unset is NOT sufficient: PlatformIO then AUTO-PICKS a
serial port, and with Light off the bench it will happily hand CanTick's port to
bossac, which starts writing SAM-BA pages at it. Observed, not theorised.

So: find a port whose hardware ID carries Light's VID. If there is exactly one,
use it. If there is none, ABORT with a message that says why -- an upload that
cannot find its board must fail loudly, never guess.

Wired in via `extra_scripts = pre:tools/select_port.py`.
"""

Import("env")  # noqa: F821  (injected by SCons/PlatformIO)

from SCons.Script import COMMAND_LINE_TARGETS, Exit  # noqa: E402
from platformio.device.list.util import list_serial_ports  # noqa: E402

# A `pre:` script runs for EVERY target, so gate on the ones that actually open a
# port. Without this, `pio run` (a plain build, which needs no hardware at all)
# would abort whenever Light was off the bench -- and a build that fails because
# a board is unplugged is its own kind of wrong.
_PORT_TARGETS = {"upload", "monitor", "uploadfs", "uploadfsota"}

# Seeed Wio Terminal, as declared by the board manifest's `hwids`. Recorded from
# what actually enumerates -- never hardcoded from memory (see LightDock-TODO,
# Phase 0). The SAM-BA bootloader presents a different PID on the same VID, so
# match on the VENDOR and let the product ID vary.
LIGHT_VID = "2886"

# Anything else expected on this bench, purely so the failure message can name it
# instead of leaving you guessing.
KNOWN_STRANGERS = {
    "303A": "CanTick (Espressif)",
    "10C4": "CP210x USB-serial bridge",
    "1A86": "CH34x USB-serial bridge",
}


def _hwid(port):
    return (getattr(port, "hwid", None) or port.get("hwid", "") or "").upper()


def _dev(port):
    return getattr(port, "device", None) or port.get("port", "")


def main():
    if not _PORT_TARGETS.intersection(COMMAND_LINE_TARGETS):
        return  # a build, a test, a clean -- nothing here opens a port

    ports = list_serial_ports()

    ours = [p for p in ports if f"VID:PID={LIGHT_VID}" in _hwid(p)]

    if len(ours) == 1:
        dev = _dev(ours[0])
        print(f"[port] Scottina Light found on {dev} (VID {LIGHT_VID})")
        env.Replace(UPLOAD_PORT=dev, MONITOR_PORT=dev)  # noqa: F821
        return

    if len(ours) > 1:
        names = ", ".join(_dev(p) for p in ours)
        print(f"[port] ERROR: more than one Seeed device attached ({names}).")
        print("[port] Disconnect one, or name the board explicitly:")
        print("[port]     pio run -t upload --upload-port <port>")
        Exit(1)

    # None. Say what IS on the bus, so the next move is obvious.
    print(f"[port] ERROR: no device with USB VID {LIGHT_VID} (Seeed Wio Terminal) is attached.")
    if ports:
        print("[port] Serial ports currently present:")
        for p in ports:
            hw = _hwid(p)
            who = ""
            for vid, label in KNOWN_STRANGERS.items():
                if f"VID:PID={vid}" in hw:
                    who = f"  <-- {label}, NOT Light"
                    break
            print(f"[port]     {_dev(p)}  {hw or '(no hardware id)'}{who}")
    else:
        print("[port] No serial ports present at all.")
    print("[port] Refusing to guess: flashing the wrong board is how CanTick gets")
    print("[port] a Wio Terminal image. Plug Light in, or pass --upload-port.")
    Exit(1)


main()
