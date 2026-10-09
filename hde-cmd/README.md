# HDE Cmd (`hde-cmd`)

HDE's own terminal emulator. Its code is deliberately kept in this folder: the window, PTY process, optional VTE
adapter and the VT screen model are all here, with a separate Makefile for building the app by itself.

The **built-in VT engine is the default** and has no VTE dependency. At build time, the Makefiles detect `vte-2.91` if
its development package is installed; `hde-cmd --vte` then opts into that backend. A build without VTE still contains a
working terminal.

## Run it

```text
hde-cmd                              start the user's login shell
hde-cmd -C ~/src                     start in a folder
hde-cmd --directory ~/src            the same, with a long option
hde-cmd --title "Build"              choose the initial window title
hde-cmd -- make check                run a command and its arguments
hde-cmd -e git status                the same, without the -- separator
hde-cmd --vte                        use VTE instead of the built-in engine (if available)
hde-cmd --help, --version
```

The built-in engine creates a PTY, sets `TERM=xterm-256color`, and supports Unicode text, ANSI/VT cursor movement,
scrolling, insert/erase, 16/256/true-colour SGR, bold/italic/underline/inverse, alternate screen, OSC window titles,
status replies, application cursor keys, mouse reporting, bracketed paste and up to 2,000 lines of scrollback.
Right-click opens the actions menu (Copy, Paste, new terminal, open a folder in a new terminal, zoom and full screen).
The header bar has a visible new-terminal button. `Ctrl+Shift+T` opens a new terminal window, `Ctrl+Shift+O` opens a
folder in a new terminal, and `Ctrl+Shift+W` closes the current window. `Ctrl+Shift+C` copies a selection,
`Ctrl+Shift+V` pastes, `Shift+PageUp/PageDown` scrolls, `Ctrl+Plus/Minus/0` changes font size, and `F11` toggles full
screen.

## Build

```sh
# From the repository root: builds build/hde-cmd with the rest of HDE
make build/hde-cmd

# This folder alone (needs GTK 3 development files; VTE remains optional)
make -C hde-cmd
./hde-cmd/hde-cmd --version

# The VT parser and screen model have no GTK/display dependency
make -C hde-cmd check
```

Dependencies: GTK 3 and the system's `libutil` library for PTY support (the library is included with the C development
packages on the supported distributions). Optional VTE: `libvte-2.91-dev` on Debian/Ubuntu or `vte291-devel` on Fedora.

## Files

| File | Purpose |
| --- | --- |
| `src/main.c` | CLI options and help/version output. |
| `src/ui.c` | GTK window, PTY lifecycle, input, drawing, clipboard and the optional VTE adapter. |
| `src/vt.h`, `src/vt.c` | Display-independent VT screen model and escape-sequence parser. |
| `Makefile` | Standalone build/install for this app. |
| `hde-cmd.desktop` | Application-menu entry, installed by the standalone and top-level builds. |
| `../tests/cmd-vt-test.c` | Plain-C parser tests run by `make check-unit` and `make -C hde-cmd check`. |
