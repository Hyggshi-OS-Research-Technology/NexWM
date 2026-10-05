#!/usr/bin/env python3
"""xtool.py — test helper for HDE in Xvfb (needs only python3 + libX11, uses ctypes).

  xtool.py popups              number of visible override-redirect windows (menus, OSD, notification popups)
  xtool.py xsettings           print the XSETTINGS values currently published (name=value)
  xtool.py root-window PROP    print the XID stored in the WINDOW property PROP of the root window (0 if absent)
"""
import ctypes
import ctypes.util
import struct
import sys

x = ctypes.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
x.XOpenDisplay.restype = ctypes.c_void_p
x.XOpenDisplay.argtypes = [ctypes.c_char_p]
x.XDefaultRootWindow.restype = ctypes.c_ulong
x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
x.XInternAtom.restype = ctypes.c_ulong
x.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
x.XGetSelectionOwner.restype = ctypes.c_ulong
x.XGetSelectionOwner.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
x.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
                         ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)),
                         ctypes.POINTER(ctypes.c_uint)]
x.XFree.argtypes = [ctypes.c_void_p]
x.XGetWindowProperty.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_long, ctypes.c_long,
                                 ctypes.c_int, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
                                 ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_ulong),
                                 ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_void_p)]


class XWindowAttributes(ctypes.Structure):
    _fields_ = [("x", ctypes.c_int), ("y", ctypes.c_int), ("width", ctypes.c_int), ("height", ctypes.c_int),
                ("border_width", ctypes.c_int), ("depth", ctypes.c_int), ("visual", ctypes.c_void_p),
                ("root", ctypes.c_ulong), ("c_class", ctypes.c_int), ("bit_gravity", ctypes.c_int),
                ("win_gravity", ctypes.c_int), ("backing_store", ctypes.c_int),
                ("backing_planes", ctypes.c_ulong), ("backing_pixel", ctypes.c_ulong),
                ("save_under", ctypes.c_int), ("colormap", ctypes.c_ulong), ("map_installed", ctypes.c_int),
                ("map_state", ctypes.c_int), ("all_event_masks", ctypes.c_long),
                ("your_event_mask", ctypes.c_long), ("do_not_propagate_mask", ctypes.c_long),
                ("override_redirect", ctypes.c_int), ("screen", ctypes.c_void_p)]


x.XGetWindowAttributes.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(XWindowAttributes)]

ERRHANDLER = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)
_ignore = ERRHANDLER(lambda d, e: 0)
x.XSetErrorHandler.argtypes = [ERRHANDLER]

d = x.XOpenDisplay(None)
if not d:
    print("cannot open display", file=sys.stderr)
    sys.exit(2)
x.XSetErrorHandler(_ignore)
root = x.XDefaultRootWindow(d)
IS_VIEWABLE = 2


def get_prop(win, name, req_type=0):
    atom = x.XInternAtom(d, name.encode(), 0)
    t, f, n, after, data = ctypes.c_ulong(), ctypes.c_int(), ctypes.c_ulong(), ctypes.c_ulong(), ctypes.c_void_p()
    if x.XGetWindowProperty(d, win, atom, 0, 1 << 20, 0, req_type, ctypes.byref(t), ctypes.byref(f),
                            ctypes.byref(n), ctypes.byref(after), ctypes.byref(data)) != 0 or not data.value:
        return None, 0
    fmt = f.value
    if fmt == 8:
        raw = ctypes.string_at(data.value, n.value)
    elif fmt == 32:
        raw = list((ctypes.c_ulong * n.value).from_address(data.value))
    else:
        raw = ctypes.string_at(data.value, n.value * 2)
    x.XFree(data)
    return raw, fmt


def popups():
    r, p, kids, nk = ctypes.c_ulong(), ctypes.c_ulong(), ctypes.POINTER(ctypes.c_ulong)(), ctypes.c_uint()
    x.XQueryTree(d, root, ctypes.byref(r), ctypes.byref(p), ctypes.byref(kids), ctypes.byref(nk))
    count = 0
    for i in range(nk.value):
        a = XWindowAttributes()
        if x.XGetWindowAttributes(d, kids[i], ctypes.byref(a)) and a.map_state == IS_VIEWABLE \
                and a.override_redirect and a.width > 8 and a.height > 8:
            count += 1
    if nk.value:
        x.XFree(kids)
    return count


def xsettings():
    owner = x.XGetSelectionOwner(d, x.XInternAtom(d, b"_XSETTINGS_S0", 0))
    if not owner:
        return None
    raw, _ = get_prop(owner, "_XSETTINGS_SETTINGS")
    if not raw:
        return {}
    e = "<" if raw[0] == 0 else ">"
    serial, n = struct.unpack(e + "II", raw[4:12])
    pos, out = 12, {"__serial__": serial}
    pad = lambda v: (v + 3) & ~3
    for _ in range(n):
        typ = raw[pos]
        nl = struct.unpack(e + "H", raw[pos + 2:pos + 4])[0]
        name = raw[pos + 4:pos + 4 + nl].decode()
        pos = pos + 4 + pad(nl) + 4
        if typ == 0:
            out[name] = struct.unpack(e + "i", raw[pos:pos + 4])[0]
            pos += 4
        elif typ == 1:
            vl = struct.unpack(e + "I", raw[pos:pos + 4])[0]
            out[name] = raw[pos + 4:pos + 4 + vl].decode()
            pos += 4 + pad(vl)
        else:
            out[name] = struct.unpack(e + "HHHH", raw[pos:pos + 8])
            pos += 8
    return out


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "popups":
        print(popups())
    elif cmd == "xsettings":
        s = xsettings()
        if s is None:
            print("NO-XSETTINGS-MANAGER")
            sys.exit(1)
        for k, v in s.items():
            print(f"{k}={v}")
    elif cmd == "root-window":
        raw, fmt = get_prop(root, sys.argv[2])
        print(hex(raw[0]) if raw and fmt == 32 else "0")
    else:
        print(__doc__)
        sys.exit(2)
