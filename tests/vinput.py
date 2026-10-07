#!/usr/bin/env python3
"""vinput.py — virtual input devices for tests/input-test.sh (python3-evdev, run as root for /dev/uinput).

  vinput.py serve FIFO RESPONSES   create / drive devices on request. One command per line is read from the named
                                   pipe FIFO; "ok <command>" or "error <command>: <reason>" is appended to RESPONSES.
      add-touchpad NAME    a clickpad like a laptop touchpad (multi-touch, two-finger scrolling, 100 x 66 mm)
      add-mouse NAME       a wheel mouse
      remove NAME          unplug the device
      swipe NAME up|down   two-finger swipe of 30 mm on a touchpad (up = the fingers move towards the screen)
      wheel NAME up|down   three wheel clicks (up = the wheel turns away from the user)
      quit
"""
import sys
import time

from evdev import AbsInfo, UInput, ecodes as e

BUS_USB, BUS_I2C = 0x03, 0x18
devices = {}
tracking_id = [100]


def add_touchpad(name):
    res = 30  # units per mm
    caps = {
        e.EV_KEY: [e.BTN_LEFT, e.BTN_TOOL_FINGER, e.BTN_TOOL_DOUBLETAP, e.BTN_TOOL_TRIPLETAP, e.BTN_TOUCH],
        e.EV_ABS: [
            (e.ABS_X, AbsInfo(0, 0, 3000, 0, 0, res)),
            (e.ABS_Y, AbsInfo(0, 0, 2000, 0, 0, res)),
            (e.ABS_MT_SLOT, AbsInfo(0, 0, 4, 0, 0, 0)),
            (e.ABS_MT_POSITION_X, AbsInfo(0, 0, 3000, 0, 0, res)),
            (e.ABS_MT_POSITION_Y, AbsInfo(0, 0, 2000, 0, 0, res)),
            (e.ABS_MT_TRACKING_ID, AbsInfo(0, 0, 65535, 0, 0, 0)),
        ],
    }
    devices[name] = UInput(caps, name=name, vendor=0x1234, product=0x0001, version=1, bustype=BUS_I2C,
                           input_props=[e.INPUT_PROP_POINTER, e.INPUT_PROP_BUTTONPAD])


def add_mouse(name):
    caps = {e.EV_KEY: [e.BTN_LEFT, e.BTN_RIGHT, e.BTN_MIDDLE], e.EV_REL: [e.REL_X, e.REL_Y, e.REL_WHEEL]}
    devices[name] = UInput(caps, name=name, vendor=0x1234, product=0x0002, version=1, bustype=BUS_USB)


def swipe(name, direction):
    ui = devices[name]
    y0, y1 = (1500, 600) if direction == "up" else (600, 1500)
    xs = (1100, 1700)
    for slot, x in enumerate(xs):
        tracking_id[0] += 1
        ui.write(e.EV_ABS, e.ABS_MT_SLOT, slot)
        ui.write(e.EV_ABS, e.ABS_MT_TRACKING_ID, tracking_id[0])
        ui.write(e.EV_ABS, e.ABS_MT_POSITION_X, x)
        ui.write(e.EV_ABS, e.ABS_MT_POSITION_Y, y0)
    ui.write(e.EV_KEY, e.BTN_TOUCH, 1)
    ui.write(e.EV_KEY, e.BTN_TOOL_DOUBLETAP, 1)
    ui.write(e.EV_ABS, e.ABS_X, xs[0])
    ui.write(e.EV_ABS, e.ABS_Y, y0)
    ui.syn()
    time.sleep(0.04)
    steps = 45
    for i in range(1, steps + 1):
        y = y0 + (y1 - y0) * i // steps
        for slot in range(len(xs)):
            ui.write(e.EV_ABS, e.ABS_MT_SLOT, slot)
            ui.write(e.EV_ABS, e.ABS_MT_POSITION_Y, y)
        ui.write(e.EV_ABS, e.ABS_Y, y)
        ui.syn()
        time.sleep(0.01)
    time.sleep(0.04)
    for slot in range(len(xs)):
        ui.write(e.EV_ABS, e.ABS_MT_SLOT, slot)
        ui.write(e.EV_ABS, e.ABS_MT_TRACKING_ID, -1)
    ui.write(e.EV_KEY, e.BTN_TOUCH, 0)
    ui.write(e.EV_KEY, e.BTN_TOOL_DOUBLETAP, 0)
    ui.syn()


def wheel(name, direction):
    ui = devices[name]
    for _ in range(3):
        ui.write(e.EV_REL, e.REL_WHEEL, 1 if direction == "up" else -1)
        ui.syn()
        time.sleep(0.05)


def run(cmd):
    op, _, rest = cmd.partition(" ")
    if op == "add-touchpad":
        add_touchpad(rest)
    elif op == "add-mouse":
        add_mouse(rest)
    elif op == "remove":
        devices.pop(rest).close()
    elif op in ("swipe", "wheel"):
        name, _, direction = rest.rpartition(" ")
        if direction not in ("up", "down"):
            raise ValueError("direction must be up or down")
        (swipe if op == "swipe" else wheel)(name, direction)
    else:
        raise ValueError("unknown command")


def serve(fifo, responses):
    with open(responses, "a", buffering=1) as out:
        while True:
            with open(fifo) as f:          # blocks until a writer opens the pipe; EOF when it closes it
                for line in f:
                    cmd = line.strip()
                    if not cmd:
                        continue
                    if cmd == "quit":
                        for ui in devices.values():
                            ui.close()
                        out.write("ok quit\n")
                        return
                    try:
                        run(cmd)
                        out.write(f"ok {cmd}\n")
                    except Exception as ex:  # noqa: BLE001
                        out.write(f"error {cmd}: {ex!r}\n")


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "serve":
        serve(sys.argv[2], sys.argv[3])
    else:
        print(__doc__)
        sys.exit(2)
