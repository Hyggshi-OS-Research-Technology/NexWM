# NexWM — HDE's own window manager

NexWM is the window manager written *in* this repository: HDE's own, under HDE's own name. It is one program with two
sides, and the session picks the side it needs:

| | |
|---|---|
| `nexwm --x11` (the default when `$DISPLAY` is set) | a **window manager**: it puts the frames around windows, gives them the focus, keeps the workspaces, moves, snaps, maximizes, closes windows, and honours the struts of HDE's panel. It speaks to the X server through XCB directly — no libX11, no toolkit — and implements the EWMH/ICCCM parts a session and its programs ask for |
| `nexwm --wayland` | the **compositor** of the "NexWM (Wayland)" session (wlroots), i.e. the program that owns the display there. This is the next step of the work: `nexwm --wayland` prints what this build has and where it is going until then |

Both were built for HDE first: the panel above windows, the workspaces Super+1…9 are used for, the keys HDE publishes,
and a configuration file the user edits.

```
nexwm                      the X11 window manager (when $DISPLAY is set)
nexwm --x11                the X11 window manager, whatever the environment says
nexwm --wayland            the Wayland compositor (a build made with wlroots)
nexwm --replace            take the window manager role over from the one that is running (X11)
nexwm --config FILE        another configuration file
nexwm --version, --help
```

Exit status: `0` the window manager ran and left cleanly, `2` the configuration file has a mistake, `3` there is no
display (or another window manager holds the screen and `--replace` was not given), `4` this build has no Wayland
compositor.

## Where it is used

* **Settings → Window Management**: NexWM is in the list next to Metacity, Openbox and the others. It supports
  `--replace`, so it takes the screen over from the running window manager and the switch needs no logout.
* **The login screen**: the **NexWM** entry (`/usr/share/xsessions/nexwm.desktop`) is the HDE session started with
  `hde-start --wm nexwm` — HDE with its own window manager from the first window on.
* On the command line, in a running HDE session: `nexwm --replace &`.

## The configuration file

`~/.config/hde/nexwm.conf` (or `$XDG_CONFIG_HOME/hde/nexwm.conf`; `HDE_NEXWM_CONF` overrides both, which is what the
tests use). A file that is not there is not a mistake: the defaults are used. Lines starting with `#` are comments.
A mistake in the file is reported with its line number, and the session log says what is wrong.

```
border 2                       # the frame around a window, in pixels (0 = none, up to 64)
focus click                    # click to focus (the default), or "focus mouse" to follow the pointer
desktops 4                     # how many workspaces
colors 0x2a2a2a 0x3a86ff       # the frame of a window that is not focused, and of the one that is
key Super+Return spawn xterm   # a key binding: Super, Ctrl, Alt and Shift, then a key
```

The actions of a `key` line:

| Action | What it does |
|--------|--------------|
| `spawn CMD` | runs `CMD` with `/bin/sh -c`. If the program is not there, the log says which one (and which file to change) |
| `close` / `kill` | asks the focused window to close (ICCCM `WM_DELETE_WINDOW`), or ends it right away |
| `next` / `prev` | the next / previous window of this workspace |
| `workspace N` | goes to workspace `N` (1 … 16). The file counts from 1, the protocol from 0 |
| `move-to N` | moves the focused window to workspace `N` |
| `maximize` / `unmaximize` | the work area (what a panel's struts leave of the screen), or the place from before |
| `fullscreen` | the whole screen (also leaves it again) |
| `snap left/right/up/down` | half of the work area on that side |
| `quit` | leaves the window manager (the session then sees a clean exit and does not restart it) |

A key may be named the X11 way: `a` … `z`, `0` … `9`, the punctuation on the keyboard (`key Super++ spawn …` for the
`+` key), `Return`, `Tab`, `BackSpace`, `Escape`, `space`, `Insert`, `Delete`, `Home`, `End`, `Page_Up`, `Page_Down`,
`Left`, `Right`, `Up`, `Down`, `F1` … `F12`, and the multimedia keys (`XF86AudioMute`, `XF86AudioRaiseVolume`,
`XF86AudioLowerVolume`, `XF86AudioPlay`, `XF86AudioNext`, `XF86AudioPrev`, `XF86AudioStop`, `XF86MonBrightnessUp/Down`,
`XF86Display`, `XF86Sleep`, `Print`, `Menu`). A key name nobody knows is refused by name, with the line number.

The defaults are:

| Keys | |
|------|--|
| `Super+Return` | a terminal (`x-terminal-emulator`) |
| `Super+E` | Hyggshi Files (`hde-files`) |
| `Super+Q` / `Super+Shift+Q` | close the focused window / leave the window manager |
| `Super+Tab` / `Super+Shift+Tab` | the next / previous window |
| `Super+1` … `Super+9` | the workspaces |
| `Super+Shift+1` … `Super+Shift+9` | move the focused window to that workspace |
| `Super+Left` / `Super+Right` | snap to the left / right half of the work area |
| `Super+Up` / `Super+Down` | maximize / unmaximize |
| `Super+F` | full screen |

What a running NexWM listens to is published on the root window, so Settings (or `xprop`) can show it:

```
xprop -root _NEXWM_KEYS        # "Super+Q close\0Super+9 workspace 3\0…", one binding per NUL-terminated line
```

## What the X11 side implements

* **The window manager role**: the `WM_Sn` selection, `SUBSTRUCTURE_REDIRECT` on the root window, and `--replace`
  (it clears the selection and waits for the window manager that is there to give the screen up).
* **EWMH**: `_NET_SUPPORTED`, `_NET_SUPPORTING_WM_CHECK` (+ `_NET_WM_NAME` = "NexWM" — that is what
  *Settings → About* shows), `_NET_CLIENT_LIST`(`_STACKING`), `_NET_ACTIVE_WINDOW`, `_NET_NUMBER_OF_DESKTOPS`,
  `_NET_DESKTOP_NAMES`, `_NET_CURRENT_DESKTOP`, `_NET_DESKTOP_GEOMETRY`, `_NET_DESKTOP_VIEWPORT`, `_NET_WM_DESKTOP`,
  `_NET_WORKAREA`, `_NET_WM_WINDOW_TYPE*`, `_NET_WM_STATE*` (maximized, full screen, hidden, skip taskbar/pager),
  `_NET_WM_STRUT(_PARTIAL)`, `_NET_FRAME_EXTENTS`, `_NET_WM_ALLOWED_ACTIONS`, `_NET_CLOSE_WINDOW`,
  `WM_CHANGE_STATE`, `WM_PROTOCOLS`/`WM_DELETE_WINDOW`, `WM_STATE`.
* **Frames**: the border around a window is drawn by NexWM (no title bar yet — that is the next step, and it is why
  the GTK window managers are still the default in HDE). A window's own size is what the program asked for; the frame
  around it is `border` pixels on each side, which is what `_NET_FRAME_EXTENTS` tells the programs.
* **Windows HDE knows about**: the panel and the desktop are docks/desktop windows: they are not framed, they are
  marked `_NET_WM_STATE_SKIP_TASKBAR`/`SKIP_PAGER`, their `_NET_WM_STRUT(_PARTIAL)` shrinks the work area, and a
  maximized window stops at the work area instead of going under the panel.
* **Workspaces**: a window of another workspace is unmapped, not destroyed (coming back is instant and the program
  keeps everything it had). A dock is on all of them.
* **Screens**: a resolution change (RandR, F8 in HDE) is noticed through the root window and the work area is
  recomputed.

Not there yet (each one is a step of its own): title bars drawn with the GTK theme, moving and resizing a window with
the mouse, window previews in the taskbar, and the Wayland compositor.

## The Wayland side

A compositor is a different program from a window manager: it owns the display, it draws every window itself, and the
input devices of the machine talk to it. NexWM builds it on **wlroots** — the library labwc, sway and HDE's own
layer-shell panels are written with — so that the same configuration file, the same key bindings and the same
`_NEXWM_KEYS` idea can be used on both sides. wlroots is only needed for `--wayland`: a build without it still has the
X11 window manager, and the program says which one it has.

`--wayland` is the door today: it prints what this build has and returns 4. The compositor itself, the session entry
`NexWM (Wayland)`, and `hde-session --wayland` preferring it over labwc come with it. Until then the Wayland session
of HDE is the "HDE (Wayland)" entry, which runs labwc.

## Building and testing it

```
make build/nexwm            # the program (XCB and wlroots are picked up by pkg-config, both optional)
make build/nexwm-test       # the configuration and key bindings, no display needed
make build/nexwm-client     # the window the shell test puts on the screen
make check-unit             # every test that needs no display, nexwm-test among them
make check-nexwm            # NexWM in Xvfb: frames, keys, workspaces, panel struts, --replace (tests/nexwm-test.sh)
```

Build dependencies: `libxcb1-dev` (Debian/Ubuntu), `libxcb-devel` (Fedora/openSUSE) — and `libwlroots-dev` for the
Wayland side. Running the shell test needs `xvfb xdotool x11-utils` (and `metacity` for the `--replace` part).
