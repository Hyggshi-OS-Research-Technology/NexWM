# NexWM — Nex Window Manager

A modern, lightweight, modular X11 window manager written in C.
Part of the **Nex / Hyggshi OS** ecosystem.

## Features

- **Lightweight & Fast**: Written in C with minimal dependencies
- **Event-driven**: No polling, no busy-loops
- **Modular Architecture**: Clean separation of concerns
- **XCB-based**: Modern asynchronous X11 protocol binding
- **EWMH/ICCCM Compliant**: Works with standard X11 applications
- **Tiling & Floating**: Support for both layout modes
- **Workspaces**: 9 virtual desktops by default
- **Multi-monitor**: Extensible monitor support
- **Keyboard-driven**: Extensive configurable keybindings
- **Window Rules**: Automatic window placement and behavior
- **Customizable Wallpaper**: Change the desktop background to any image with scale/stretch/center/tile modes from the NexSettings control panel or the `nex-wallpaper` CLI
- **Built-in Terminal (NexTerminal)**: A Qt6 terminal emulator that spawns a real shell over a pseudo-terminal (pty), with color support, copy/paste, and resizable TUI apps — launched via `Super + Enter` or the `nex-terminal` launcher entry (`nex-terminal.desktop`)

## Architecture

```
┌─────────────────────────────────────┐
│           main.c                    │
│    (CLI parsing, initialization)    │
└──────────────┬──────────────────────┘
               │
┌──────────────▼──────────────────────┐
│           wm.c / wm.h               │
│    (X11 connection, event loop)     │
└──────────────┬──────────────────────┘
               │
    ┌──────────┼──────────┬───────────┬──────────┐
    │          │          │           │          │
┌───▼───┐ ┌───▼───┐ ┌───▼───┐  ┌───▼───┐ ┌───▼───┐
│events │ │client │ │keybind│  │ewmh   │ │monitor│
└───────┘ └───────┘ └───────┘  └───────┘ └───────┘
    │          │          │           │          │
┌───▼───┐ ┌───▼───┐ ┌───▼───┐  ┌───▼───┐ ┌───▼───┐
│focus  │ │layout │ │rules  │  │ipc    │ │workspace
└───────┘ └───────┘ └───────┘  └───────┘ └───────┘
```

## Dependencies

- `gcc` / `g++` or `clang` / `clang++`
- `make`
- `libxcb`
- `Qt6` (Widgets, Gui, Core for GUI components: `nex-launcher`, `nex-settings`, `nex-notify`)

### Installing Dependencies

**Debian/Ubuntu:**
```bash
sudo apt install libx11-dev libxcb1-dev qt6-base-dev
```

**Arch Linux:**
```bash
sudo pacman -S libxcb xcb-util xcb-util-randr xcb-util-ewmh \
               xcb-util-icccm xcb-util-keysyms
```

**Fedora:**
```bash
sudo dnf install libxcb-devel xcb-util-devel xcb-util-randr-devel \
                 xcb-util-ewmh-devel xcb-util-icccm-devel xcb-util-keysyms-devel
```

## Build

```bash
make              # Standard build
make debug        # Debug build with sanitizers
make release      # Optimized release build
make clean        # Clean build artifacts
```

## Installation

```bash
sudo make install    # Install to /usr/local (default)
sudo make uninstall  # Remove installed files
```

## Running NexWM

### Testing with Xephyr (recommended for development)

```bash
# Terminal 1: Start Xephyr
Xephyr :1 -ac -br -noreset -screen 1280x720 &

# Terminal 2: Run NexWM
DISPLAY=:1 ./bin/nexwm --debug

# Terminal 3: Run applications
DISPLAY=:1 xterm
DISPLAY=:1 xclock
```

### Running as your main WM

Add to your `~/.xinitrc`:
```bash
exec nexwm
```

## Configuration

Configuration file: `~/.config/nexwm/nexwm.conf`

See `config/nexwm.conf` for an example configuration.

### Appearances

| Key | Value | Description |
|-----|-------|-------------|
| `border_width` | `2` | Thickness of the window border (px) |
| `border_focus` | `#5b8dd9` | Border color of the focused window |
| `border_normal` | `#444444` | Border color of unfocused windows |
| `gaps` | `8` | Gap between tiled windows (px) |
| `wallpaper` | `/path/to/image.png` | Desktop background image (empty = none / solid color) |
| `wallpaper_mode` | `scale` | How the image is displayed: `scale` \| `stretch` \| `center` \| `tile` |

> **Tip:** The easiest way to change the background is through the **NexSettings** panel
> (`nex-settings`) — pick *Background Image* with the **Browse…** button, choose a *Background
> Mode*, and click **Apply & Save**. The selection is saved to `nexwm.conf` and re-applied on
> the next session via `nex-wallpaper --restore`. You can also change it from the terminal:
>
> ```bash
> nex-wallpaper --set /path/to/image.png --mode scale
> nex-wallpaper --color 0x1a1a2e      # solid color background
> ```

## Default Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| `Super + Enter` | Launch terminal |
| `Super + D` | Launch application launcher |
| `Super + Q` | Close focused window |
| `Super + Space` | Toggle floating |
| `Super + 1-9` | Switch to workspace |
| `Super + Shift + 1-9` | Move window to workspace |
| `Super + Tab` | Focus next window |
| `Super + Shift + Tab` | Focus previous window |
| `Super + Shift + Escape` | Exit NexWM |

*Note: Maximize (`Max`) and Fullscreen (`Full`) controls are accessible via panel buttons on the status bar or via `nexwmctl maximize`/`nexwmctl fullscreen`.*

## Development Roadmap

| Phase | Feature | Status |
|-------|---------|--------|
| 1 | X11 connection + event loop + MapRequest | In Progress |
| 2 | Client management + focus + move + resize | Planned |
| 3 | Keyboard shortcuts | Planned |
| 4 | Floating layout | Planned |
| 5 | Tiling layout | Planned |
| 6 | Workspaces | Planned |
| 7 | EWMH/ICCCM | Planned |
| 8 | RandR/multi-monitor | Planned |
| 9 | Configuration file parser | Planned |
| 10 | IPC + nexwmctl | Planned |
| 11 | Testing + memory safety | Planned |

## License

MIT License - See LICENSE file for details.
