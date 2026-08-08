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

- `gcc` or `clang`
- `make`
- `libxcb`
- `xcb-util`
- `xcb-randr`
- `xcb-ewmh`
- `xcb-icccm`
- `xcb-keysyms`

### Installing Dependencies

**Debian/Ubuntu:**
```bash
sudo apt install libx11-dev libxcb1-dev libxcb-util-dev libxcb-randr0-dev \
                 libxcb-ewmh-dev libxcb-icccm4-dev libxcb-keysyms1-dev
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

## Default Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| `Super + Enter` | Launch terminal |
| `Super + D` | Launch application launcher |
| `Super + Q` | Close focused window |
| `Super + F` | Toggle fullscreen |
| `Super + M` | Toggle maximize |
| `Super + Space` | Toggle floating |
| `Super + 1-9` | Switch to workspace |
| `Super + Shift + 1-9` | Move window to workspace |
| `Super + Tab` | Focus next window |
| `Super + Shift + Tab` | Focus previous window |
| `Super + Shift + Escape` | Exit NexWM |

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
