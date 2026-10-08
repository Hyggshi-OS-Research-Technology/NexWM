# NexWM — HDE's own window manager

NexWM is the window manager written *in* this repository: HDE's own, under HDE's own name. It is one program with two
sides, and the session picks the side it needs:

| | |
|---|---|
| `nexwm --x11` (the default when `$DISPLAY` is set) | a **window manager**: it puts the frames around windows, gives them the focus, keeps the workspaces, moves, snaps, maximizes, closes windows, and honours the struts of HDE's panel. It speaks to the X server through XCB directly — no libX11, no toolkit — and implements the EWMH/ICCCM parts a session and its programs ask for |
| `nexwm --wayland` | the **Wayland compositor** of the additive "NexWM (Wayland)" login session (wlroots): it owns the display, manages XDG toplevels and popups, arranges layer-shell surfaces, handles input and workspaces, and publishes window state to HDE's Wayland taskbar |

Both were built for HDE first: the panel above windows, the workspaces Super+1…9 are used for, the keys HDE publishes,
and a configuration file the user edits.

```
nexwm                      the X11 window manager (when $DISPLAY is set)
nexwm --x11                the X11 window manager, whatever the environment says
nexwm --wayland            the Wayland compositor (a build made with wlroots)
nexwm --wayland --session CMD  start CMD inside this compositor (used by hde-session)
nexwm --replace            take the window manager role over from the one that is running (X11)
nexwm --config FILE        another configuration file
nexwm --version, --help
```

Exit status: `0` the compositor/window manager or its session exited cleanly, `2` the configuration file has a mistake,
`3` the display/runtime directory is not available (or an X11 window manager holds the screen and `--replace` was not
given), `4` this build has no Wayland compositor.

## Where it is used

* **Settings → Window Management**: NexWM is in the list next to Metacity, Openbox and the others. On X11 it supports
  `--replace`, so it takes over from the running window manager without a logout.
* **The login screen**: **NexWM** (`/usr/share/xsessions/nexwm.desktop`) is the X11 session; **NexWM (Wayland)**
  (`/usr/share/wayland-sessions/nexwm-wayland.desktop`) is the additive Wayland session. The existing **HDE (Wayland)**
  entry stays on labwc. `hde-session --wayland` prefers NexWM's compositor when this build has wlroots and otherwise
  starts labwc.
* On the command line, in a running X11 HDE session: `nexwm --replace &`. A Wayland compositor cannot replace the
  compositor that owns its display.

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

A key may be named the X11 way: `a` … `z` (letter case is ignored; write `Shift` explicitly when it is part of the
shortcut), `0` … `9`, the punctuation on the keyboard (`key Super++ spawn …` for the `+` key), `Return`, `Tab`,
`BackSpace`, `Escape`, `space`, `Insert`, `Delete`, `Home`, `End`, `Page_Up`, `Page_Down`,
`Left`, `Right`, `Up`, `Down`, `F1` … `F12`, and the multimedia keys (`XF86AudioMute`, `XF86AudioRaiseVolume`,
`XF86AudioLowerVolume`, `XF86AudioPlay`, `XF86AudioNext`, `XF86AudioPrev`, `XF86AudioStop`, `XF86MonBrightnessUp/Down`,
`XF86Display`, `XF86Sleep`, `Print`, `Menu`). A key name nobody knows is refused by name, with the line number.

The defaults are:

| Keys | |
|------|--|
| `Super+Return` | a terminal (`hde-choose terminal`: HDE Cmd first, then other installed terminals; asks and remembers when there is a choice) |
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

* **The window manager role**: `SUBSTRUCTURE_REDIRECT` on the root window, and the `WM_Sn` selection — taken once the
  screen is its own, never before. (Holding the selection is what being the window manager *means* in ICCCM, so a
  program that takes it from a window manager that is running is asking that one to leave: with `--replace` that is the
  point — NexWM clears the selection and waits for the screen — while a start without it refuses with status 3 and
  leaves the window manager that is there alone, instead of ending the session's window manager as a side effect.)
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

Still planned as separate steps: X11 title bars drawn with the GTK theme and X11 taskbar window previews. Wayland
uses client-side decorations and accepts move/resize requests from Wayland clients.

## The Wayland side

A Wayland compositor owns the display and receives its input; it is distinct from NexWM's X11 window-manager role.
`nexwm --wayland` uses **wlroots** (minimum supported API 0.17) for the backend, renderer, protocol implementations and
scene graph. NexWM supplies the policies: the shared `~/.config/hde/nexwm.conf`, keyboard shortcuts, focus, workspaces,
window placement and the HDE session process. wlroots is optional at build time; without it the X11 binary remains
usable and `nexwm --wayland` exits 4 with the missing package named.

The compositor publishes `wl_compositor`, `xdg_wm_base`, `zwlr_layer_shell_v1` and
`zwlr_foreign_toplevel_manager_v1`; it displays XDG toplevels/popups and layer-shell surfaces, handles pointer and
keyboard input, and reports windows to HDE's Wayland taskbar. X11 clients are not provided by this compositor (there is
no Xwayland integration yet). The X11 backend remains a separate session and continues to use XCB directly.

The additive **NexWM (Wayland)** login entry runs `nexwm --wayland --session ...`, so HDE starts on NexWM's compositor.
The existing **HDE (Wayland)** entry and labwc fallback remain intact. A Wayland compositor cannot use `--replace`:
only one compositor can own a Wayland display socket.

## Building and testing it

```
make build/nexwm                  # the program (XCB and wlroots are picked up by pkg-config, both optional)
make build/nexwm-test             # configuration and key bindings, no display needed
make build/nexwm-client           # the window the X11 shell test puts on the screen
make check-unit                   # every test that needs no display, nexwm-test among them
make check-nexwm                  # X11 in Xvfb plus the wlroots headless Wayland smoke test when available
sh tests/nexwm-wayland-test.sh    # compositor globals, --session and clean shutdown on WLR_BACKENDS=headless
```

Build dependencies: `libxcb1-dev` (Debian/Ubuntu), `libxcb-devel` (Fedora/openSUSE), and wlroots 0.17+ development
files for the Wayland side (`libwlroots-0.18-dev` on Debian trixie, `libwlroots-0.19-dev` on Debian sid, `libwlroots-dev`
on Ubuntu/older Debian, or `wlroots-devel` on Fedora). wlroots consumers must generate its XDG-shell protocol header with
`wayland-scanner` from `wayland-protocols` (`wayland-protocols-devel` on Fedora); the Makefile does this when wlroots is
available. The optional Wayland smoke test also needs `wayland-client` development files and a wlroots build with the
headless backend and pixman renderer. The X11 shell test needs `xvfb xdotool x11-utils` (and
`metacity` for its `--replace` part).

XCB and wlroots are **optional at build time**. `nexwm --version` and `nexwm --help` report each backend as *yes* or
*no*, with the package to install when it is missing. A build without `libxcb` still compiles; `nexwm --x11` explains
the missing dependency, and `make check-nexwm` runs whichever backend tests this machine can support instead of failing
just because the other backend's development files are absent.
