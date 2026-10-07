#!/usr/bin/env python3
"""tests/fake-mpv.py — a stand-in for mpv, for tests/player-window-test.sh.

It plays nothing. It writes down the arguments it was given (HDE_FAKE_MPV_LOG), opens the socket hde-media asked for
(--input-ipc-server=PATH), answers the JSON commands it is sent over it (and writes those down too), stays "playing"
for HDE_FAKE_MPV_SECONDS seconds and exits 0 — which is exactly what the player window sees when a track ends.

The player window is built against what a *real* mpv answers: {"data":...,"error":"success"} per command, one object
per line, --wid / --no-video / --really-quiet / --no-config taken as they come (it ignores everything but its own two
options). keep-open is not implemented on purpose: the window has to notice the process gone and play the next track.

Environment:
  HDE_FAKE_MPV_LOG       a file the arguments and the commands are appended to
  HDE_FAKE_MPV_SECONDS   how long it "plays" before exiting (default 3)
  HDE_FAKE_MPV_DURATION  what it answers for the duration property (default 2.5; 0 = "not known")
"""
import json
import os
import socket
import sys
import time

log = os.environ.get("HDE_FAKE_MPV_LOG")
seconds = float(os.environ.get("HDE_FAKE_MPV_SECONDS", "3"))
duration = float(os.environ.get("HDE_FAKE_MPV_DURATION", "2.5"))
args = sys.argv[1:]


def note(line):
    if not log:
        return
    try:
        with open(log, "a") as f:
            f.write(line + "\n")
    except OSError:
        pass


note("ARGS\t" + "\t".join(args))

ipc = None
for a in args:
    if a.startswith("--input-ipc-server="):
        ipc = a.split("=", 1)[1]

server = None
conn = None
if ipc:
    try:
        os.unlink(ipc)
    except OSError:
        pass
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(ipc)
    server.listen(1)
    server.settimeout(0.1)

start = time.time()
base = start          # where the "position" counts from
offset = 0.0          # ... and what it counted to when it was seeked


def answer(obj):
    if conn is not None:
        try:
            conn.sendall((json.dumps(obj) + "\n").encode("utf-8"))
        except OSError:
            pass


def handle(line):
    global offset, base
    note("CMD\t" + line)
    try:
        obj = json.loads(line)
    except ValueError:
        return
    cmd = obj.get("command") or []
    if not cmd:
        return
    if cmd[0] == "get_property":
        prop = cmd[1] if len(cmd) > 1 else ""
        if prop == "duration":
            answer({"data": duration if duration > 0 else None, "error": "success"})
        elif prop == "time-pos":
            pos = offset + (time.time() - base)
            if duration > 0:
                pos = min(pos, duration)
            answer({"data": pos, "error": "success"})
        else:
            answer({"data": None, "error": "property unavailable"})
    elif cmd[0] == "seek":
        try:
            offset = float(cmd[1])
        except (IndexError, ValueError):
            offset = 0.0
        if duration > 0:
            offset = min(offset, duration)
        base = time.time()
        answer({"error": "success"})
    else:
        # set_property pause / volume / mute: nothing to do, but the answer is expected
        answer({"error": "success"})


buf = b""
while time.time() - start < seconds:
    if server is not None and conn is None:
        try:
            conn, _ = server.accept()
            conn.settimeout(0.1)
            note("CONNECTED")
        except socket.timeout:
            pass
    if conn is not None:
        try:
            data = conn.recv(4096)
        except socket.timeout:
            data = None
        except OSError:
            data = b""
        if data:
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                handle(line.decode("utf-8", "replace"))
        elif data == b"":
            conn.close()
            conn = None
            note("CLOSED")

# the end of the track: the socket goes away with the process, the way mpv's does
if conn is not None:
    conn.close()
if server is not None:
    server.close()
if ipc:
    try:
        os.unlink(ipc)
    except OSError:
        pass
note("EXIT")
sys.exit(0)
