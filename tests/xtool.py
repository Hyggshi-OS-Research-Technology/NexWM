#!/usr/bin/env python3
"""xtool.py — test helper for HDE in Xvfb (needs only python3 + libX11, uses ctypes).

  xtool.py popups              number of visible override-redirect windows (menus, OSD, notification popups)
  xtool.py xsettings           print the XSETTINGS values currently published (name=value)
  xtool.py root-window PROP    print the XID stored in the WINDOW property PROP of the root window (0 if absent)
  xtool.py pixel X Y           print the color of the screen pixel at X,Y as "R G B" (0-255)
  xtool.py selection-owner SEL print the XID of the owner of selection SEL (e.g. CLIPBOARD), 0 if none
  xtool.py selection-targets SEL  print the targets (formats) the owner of selection SEL offers, e.g. image/png
  xtool.py scroll-watch SECONDS   map a full-screen window, count the scroll "clicks" it receives (core buttons
                               4-7, also what smooth-scrolling touchpads send to old clients); prints "ready" once
                               mapped, then "up=N down=N left=N right=N"
  xtool.py own-selection SEL SECONDS  own selection SEL for SECONDS, e.g. _XSETTINGS_S0 = "another XSETTINGS manager
                               is running" (prints "ready" once it owns it)
  xtool.py geometry XID        print "X Y WIDTH HEIGHT" of window XID as the X server has it (X, Y: on the screen)
  xtool.py cardinals XID PROP  print the numbers in property PROP of window XID, e.g. _NET_WM_STRUT_PARTIAL
  xtool.py frame XID           print "X Y WIDTH HEIGHT" of window XID with the frame the window manager drew around it
                               (_NET_FRAME_EXTENTS): where its title bar begins
  xtool.py maximize XID        ask the window manager to maximize window XID (_NET_WM_STATE, as a pager does)
  xtool.py wm-state XID        print the _NET_WM_STATE of window XID (e.g. _NET_WM_STATE_MAXIMIZED_VERT ...)
"""
import ctypes
import ctypes.util
import struct
import sys
import time

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
x.XTranslateCoordinates.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
                                    ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
                                    ctypes.POINTER(ctypes.c_ulong)]
x.XCreateSimpleWindow.restype = ctypes.c_ulong
x.XCreateSimpleWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int, ctypes.c_uint,
                                  ctypes.c_uint, ctypes.c_uint, ctypes.c_ulong, ctypes.c_ulong]
x.XConvertSelection.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong,
                                ctypes.c_ulong]
x.XCheckTypedWindowEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_void_p]
x.XGetAtomName.restype = ctypes.c_void_p
x.XGetAtomName.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
x.XFlush.argtypes = [ctypes.c_void_p]


class XImage(ctypes.Structure):
    _fields_ = [("width", ctypes.c_int), ("height", ctypes.c_int), ("xoffset", ctypes.c_int), ("format", ctypes.c_int),
                ("data", ctypes.c_void_p), ("byte_order", ctypes.c_int), ("bitmap_unit", ctypes.c_int),
                ("bitmap_bit_order", ctypes.c_int), ("bitmap_pad", ctypes.c_int), ("depth", ctypes.c_int),
                ("bytes_per_line", ctypes.c_int), ("bits_per_pixel", ctypes.c_int), ("red_mask", ctypes.c_ulong),
                ("green_mask", ctypes.c_ulong), ("blue_mask", ctypes.c_ulong)]


x.XGetImage.restype = ctypes.POINTER(XImage)
x.XGetImage.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint,
                        ctypes.c_ulong, ctypes.c_int]
ZPIXMAP = 2

x.XSelectInput.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_long]
x.XMapRaised.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
x.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
x.XPending.argtypes = [ctypes.c_void_p]
x.XNextEvent.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
x.XDisplayWidth.argtypes = [ctypes.c_void_p, ctypes.c_int]
x.XDisplayHeight.argtypes = [ctypes.c_void_p, ctypes.c_int]
x.XDestroyWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
x.XSetSelectionOwner.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong]
x.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_long, ctypes.c_void_p]


class XButtonEvent(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int),
                ("display", ctypes.c_void_p), ("window", ctypes.c_ulong), ("root", ctypes.c_ulong),
                ("subwindow", ctypes.c_ulong), ("time", ctypes.c_ulong), ("x", ctypes.c_int), ("y", ctypes.c_int),
                ("x_root", ctypes.c_int), ("y_root", ctypes.c_int), ("state", ctypes.c_uint),
                ("button", ctypes.c_uint), ("same_screen", ctypes.c_int)]


class XEvent(ctypes.Union):
    _fields_ = [("type", ctypes.c_int), ("xbutton", XButtonEvent), ("pad", ctypes.c_long * 24)]


BUTTON_PRESS = 4
BUTTON_PRESS_MASK = 1 << 2

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


def geometry(win):
    a = XWindowAttributes()
    if not x.XGetWindowAttributes(d, win, ctypes.byref(a)):
        return None
    rx, ry, child = ctypes.c_int(), ctypes.c_int(), ctypes.c_ulong()
    if not x.XTranslateCoordinates(d, win, root, 0, 0, ctypes.byref(rx), ctypes.byref(ry), ctypes.byref(child)):
        return None
    return rx.value, ry.value, a.width, a.height


def frame(win):
    g = geometry(win)
    if g is None:
        return None
    raw, fmt = get_prop(win, "_NET_FRAME_EXTENTS")
    left, right, top, bottom = (list(raw) + [0, 0, 0, 0])[:4] if raw and fmt == 32 else (0, 0, 0, 0)
    return g[0] - left, g[1] - top, g[2] + left + right, g[3] + top + bottom


def maximize(win):
    ev = (ctypes.c_long * 24)()          # XClientMessageEvent on LP64: type, serial, send_event, display, window,
    ev[0] = 33                           # message_type, format, data.l[0..4]; 33 = ClientMessage
    ev[4] = win
    ev[5] = x.XInternAtom(d, b"_NET_WM_STATE", 0)
    ev[6] = 32
    ev[7] = 1                            # _NET_WM_STATE_ADD
    ev[8] = x.XInternAtom(d, b"_NET_WM_STATE_MAXIMIZED_VERT", 0)
    ev[9] = x.XInternAtom(d, b"_NET_WM_STATE_MAXIMIZED_HORZ", 0)
    ev[10] = 2                           # source: a pager
    x.XSendEvent(d, root, 0, (1 << 19) | (1 << 20), ev)     # SubstructureNotify | SubstructureRedirect
    x.XSync(d, 0)


def activate(win):
    """Ask the window manager for a window the way HDE's panel does: _NET_ACTIVE_WINDOW (EWMH), which is what the
    taskbar of the panel sends on a click — a minimized window comes back, one of another workspace is switched to."""
    ev = (ctypes.c_long * 24)()
    ev[0] = 33                           # ClientMessage
    ev[4] = win
    ev[5] = x.XInternAtom(d, b"_NET_ACTIVE_WINDOW", 0)
    ev[6] = 32
    ev[7] = 2                            # source: a pager (the panel)
    ev[8] = 0                            # no timestamp: take the event when it comes
    ev[9] = 0                            # the window that asked (none: this is the panel)
    x.XSendEvent(d, root, 0, (1 << 19) | (1 << 20), ev)     # SubstructureNotify | SubstructureRedirect
    x.XSync(d, 0)


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


def pixel(px, py):
    img = x.XGetImage(d, root, px, py, 1, 1, 0xFFFFFFFF, ZPIXMAP)
    if not img:
        return None
    im = img.contents
    nbytes = max(1, im.bits_per_pixel // 8)
    raw = ctypes.string_at(im.data, nbytes)
    value = int.from_bytes(raw, "little" if im.byte_order == 0 else "big")

    def channel(mask):
        if not mask:
            return 0
        shift = (mask & -mask).bit_length() - 1
        top = mask >> shift
        return ((value & mask) >> shift) * 255 // top

    return channel(im.red_mask), channel(im.green_mask), channel(im.blue_mask)


def atom_name(a):
    p = x.XGetAtomName(d, a)
    if not p:
        return "?"
    name = ctypes.string_at(p).decode(errors="replace")
    x.XFree(p)
    return name


def selection_targets(sel_name, timeout=3.0):
    """Ask the owner of the selection for TARGETS (like a pasting application does)."""
    win = x.XCreateSimpleWindow(d, root, 0, 0, 1, 1, 0, 0, 0)
    prop = x.XInternAtom(d, b"HDE_XTOOL_TARGETS", 0)
    x.XConvertSelection(d, x.XInternAtom(d, sel_name.encode(), 0), x.XInternAtom(d, b"TARGETS", 0), prop, win, 0)
    x.XFlush(d)
    ev = (ctypes.c_long * 24)()          # XEvent; XSelectionEvent.property is the 8th long on LP64
    deadline = time.time() + timeout
    while time.time() < deadline:
        if x.XCheckTypedWindowEvent(d, win, 31, ev):     # SelectionNotify
            if ev[7] == 0:
                return None                              # the owner refused
            raw, fmt = get_prop(win, "HDE_XTOOL_TARGETS")
            return [atom_name(a) for a in raw] if raw and fmt == 32 else []
        time.sleep(0.05)
    return None


def scroll_watch(seconds):
    """Scroll clicks reaching a full-screen window: button 4 = up (the content moves down), 5 = down (the content
    moves up), 6 = left, 7 = right."""
    w = x.XCreateSimpleWindow(d, root, 0, 0, x.XDisplayWidth(d, 0), x.XDisplayHeight(d, 0), 0, 0, 0xFFFFFF)
    x.XSelectInput(d, w, BUTTON_PRESS_MASK)
    x.XMapRaised(d, w)
    x.XSync(d, 0)
    print("ready", flush=True)
    counts = {4: 0, 5: 0, 6: 0, 7: 0}
    ev = XEvent()
    deadline = time.time() + seconds
    while time.time() < deadline:
        while x.XPending(d):
            x.XNextEvent(d, ctypes.byref(ev))
            if ev.type == BUTTON_PRESS and ev.xbutton.button in counts:
                counts[ev.xbutton.button] += 1
        time.sleep(0.01)
    x.XDestroyWindow(d, w)
    x.XSync(d, 0)
    return "up=%d down=%d left=%d right=%d" % (counts[4], counts[5], counts[6], counts[7])


def own_selection(sel_name, seconds):
    w = x.XCreateSimpleWindow(d, root, -10, -10, 1, 1, 0, 0, 0)
    atom = x.XInternAtom(d, sel_name.encode(), 0)
    x.XSetSelectionOwner(d, atom, w, 0)          # CurrentTime
    x.XSync(d, 0)
    if x.XGetSelectionOwner(d, atom) != w:
        return False
    print("ready", flush=True)
    ev = XEvent()
    deadline = time.time() + seconds
    while time.time() < deadline:
        while x.XPending(d):
            x.XNextEvent(d, ctypes.byref(ev))   # SelectionRequest & co.: nothing to answer for the tests
        time.sleep(0.05)
    x.XDestroyWindow(d, w)
    x.XSync(d, 0)
    return True


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
    elif cmd == "pixel":
        c = pixel(int(sys.argv[2]), int(sys.argv[3]))
        if c is None:
            print("0 0 0")
            sys.exit(1)
        print("%d %d %d" % c)
    elif cmd == "selection-owner":
        owner = x.XGetSelectionOwner(d, x.XInternAtom(d, sys.argv[2].encode(), 0))
        print(hex(owner) if owner else "0")
    elif cmd == "scroll-watch":
        print(scroll_watch(float(sys.argv[2])))
    elif cmd == "own-selection":
        if not own_selection(sys.argv[2], float(sys.argv[3])):
            print("NOT-OWNED")
            sys.exit(1)
    elif cmd == "geometry":
        g = geometry(int(sys.argv[2], 0))
        if g is None:
            print("NO-WINDOW")
            sys.exit(1)
        print("%d %d %d %d" % g)
    elif cmd == "cardinals":
        raw, fmt = get_prop(int(sys.argv[2], 0), sys.argv[3])
        print(" ".join(str(v) for v in raw) if raw and fmt == 32 else "")
    elif cmd == "frame":
        g = frame(int(sys.argv[2], 0))
        if g is None:
            print("NO-WINDOW")
            sys.exit(1)
        print("%d %d %d %d" % g)
    elif cmd == "maximize":
        maximize(int(sys.argv[2], 0))
    elif cmd == "activate":
        activate(int(sys.argv[2], 0))
    elif cmd == "wm-state":
        raw, fmt = get_prop(int(sys.argv[2], 0), "_NET_WM_STATE")
        print(" ".join(atom_name(a) for a in raw) if raw and fmt == 32 else "")
    elif cmd == "selection-targets":
        t = selection_targets(sys.argv[2])
        if t is None:
            print("NO-ANSWER")
            sys.exit(1)
        print(" ".join(t))
    else:
        print(__doc__)
        sys.exit(2)
