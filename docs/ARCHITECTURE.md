# NexWM Architecture

## Overview

NexWM follows a modular, event-driven architecture built on top of XCB.

## Core Principles

1. Correctness -> Stability -> Architecture -> Performance -> Features
2. Minimal global state
3. Clear module boundaries
4. Event-driven, no polling
5. Proper resource cleanup

## Module Descriptions

| Module | Responsibility |
|--------|---------------|
| main.c | Entry point, CLI parsing |
| wm.c | X11 connection lifecycle, event loop |
| events.c | X11 event dispatching |
| client.c | Window management (create, destroy, focus, etc.) |
| workspace.c | Virtual desktops |
| monitor.c | Display management |
| layout.c | Layout engine (tiling/floating) |
| focus.c | Focus cycling |
| keybind.c | Keyboard shortcuts |
| config.c | Configuration |
| rules.c | Window matching rules |
| atoms.c | X11 atoms |
| ewmh.c | EWMH/ICCCM compliance |
| ipc.c | Inter-process communication |
| log.c | Logging system |

## Data Structures

- **Client List**: Doubly-linked list (typical <50 windows)
- **Workspaces**: Fixed-size array (max 16)
- **Monitors**: Fixed-size array (max 8)

## X11 Event Flow

```
Client calls xcb_map_window()
        |
        v
X Server -> MapRequest -> NexWM
        |
        v
Create nex_client_t -> Apply rules -> Assign workspace
        |
        v
Map window -> Focus -> Update EWMH
```
