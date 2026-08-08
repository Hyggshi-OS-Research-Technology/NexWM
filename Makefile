# NexWM Makefile
# Nex Window Manager & Desktop Suite

CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Wpedantic -std=c11 -O2 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Iinclude
LDFLAGS ?= -lxcb -lX11 $(wildcard /usr/lib/x86_64-linux-gnu/libxcb-keysyms.so*) $(wildcard /usr/lib/x86_64-linux-gnu/libxcb-util.so*) $(wildcard /usr/lib/x86_64-linux-gnu/libxcb-randr.so*) $(wildcard /usr/lib/x86_64-linux-gnu/libxcb-ewmh.so*) $(wildcard /usr/lib/x86_64-linux-gnu/libxcb-icccm.so*)

# Panel & Desktop components XCB flags
PANEL_LDFLAGS   = -lxcb -lX11
DESKTOP_LDFLAGS = -lxcb -lX11
NOTIFY_LDFLAGS  = -lxcb -lX11

# Wallpaper: XLib only (Imlib2 optional via HAVE_IMLIB2=1)
ifeq ($(HAVE_IMLIB2),1)
WALLPAPER_CFLAGS  = -DHAVE_IMLIB2
WALLPAPER_LDFLAGS = -lX11 -lImlib2
else
WALLPAPER_LDFLAGS = -lX11
endif

SAN_FLAGS ?=

DEBUG_CFLAGS = -Wall -Wextra -Wpedantic -std=c11 -g -O0 -DDEBUG \
               -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Iinclude \
               -fsanitize=address,undefined -fno-omit-frame-pointer
DEBUG_LDFLAGS = $(LDFLAGS) -fsanitize=address,undefined

SRCDIR  = src
BINDIR  = bin
PREFIX  ?= /usr/local
BINDIR_INSTALL = $(PREFIX)/bin
ETCDIR  = $(PREFIX)/etc/nexwm

# ── NexWM ─────────────────────────────────────────────────────────────────────
WM_SRCS = $(SRCDIR)/main.c \
          $(SRCDIR)/wm.c \
          $(SRCDIR)/events.c \
          $(SRCDIR)/client.c \
          $(SRCDIR)/monitor.c \
          $(SRCDIR)/workspace.c \
          $(SRCDIR)/layout.c \
          $(SRCDIR)/focus.c \
          $(SRCDIR)/keybind.c \
          $(SRCDIR)/config.c \
          $(SRCDIR)/rules.c \
          $(SRCDIR)/atoms.c \
          $(SRCDIR)/ewmh.c \
          $(SRCDIR)/ipc.c \
          $(SRCDIR)/log.c

WM_OBJS = $(WM_SRCS:.c=.o)
WM_DEPS = $(WM_SRCS:.c=.d)

# ── nexwmctl ──────────────────────────────────────────────────────────────────
CTL_SRCS = $(SRCDIR)/nexwmctl.c
CTL_OBJS = $(CTL_SRCS:.c=.o)
CTL_DEPS = $(CTL_SRCS:.c=.d)

# ── Desktop Suite Components ──────────────────────────────────────────────────
CXX         ?= g++
QT6_CFLAGS  ?= $(shell pkg-config --cflags Qt6Widgets Qt6Gui Qt6Core 2>/dev/null || echo "-I/usr/include/qt6 -I/usr/include/qt6/QtWidgets -I/usr/include/qt6/QtGui -I/usr/include/qt6/QtCore -I/usr/include/x86_64-linux-gnu/qt6 -I/usr/include/x86_64-linux-gnu/qt6/QtWidgets -I/usr/include/x86_64-linux-gnu/qt6/QtGui -I/usr/include/x86_64-linux-gnu/qt6/QtCore -fPIC")
QT6_LDFLAGS ?= $(shell pkg-config --libs Qt6Widgets Qt6Gui Qt6Core 2>/dev/null || echo "-lQt6Widgets -lQt6Gui -lQt6Core")

PANEL_DIR    = components/panel
LAUNCH_DIR   = components/launcher
WALL_DIR     = components/wallpaper
DESK_DIR     = components/desktop
NOTIFY_DIR   = components/notify
SETT_DIR     = components/settings
FM_DIR       = components/fm
SESSION_DIR  = components/session

PANEL_SRCS   = $(PANEL_DIR)/nex-panel.c
LAUNCH_SRCS  = $(LAUNCH_DIR)/nex-launcher.cpp
WALL_SRCS    = $(WALL_DIR)/nex-wallpaper.c
DESK_SRCS    = $(DESK_DIR)/nex-desktop.c
NOTIFY_SRCS  = $(NOTIFY_DIR)/nex-notify.cpp
SETT_SRCS    = $(SETT_DIR)/nex-settings.cpp
FM_SRCS      = $(FM_DIR)/nex-fm.cpp
SESSION_SRCS = $(SESSION_DIR)/nex-session.cpp

PANEL_OBJS   = $(PANEL_SRCS:.c=.o)
LAUNCH_OBJS  = $(LAUNCH_DIR)/nex-launcher.o
WALL_OBJS    = $(WALL_SRCS:.c=.o)
DESK_OBJS    = $(DESK_SRCS:.c=.o)
NOTIFY_OBJS  = $(NOTIFY_DIR)/nex-notify.o
SETT_OBJS    = $(SETT_DIR)/nex-settings.o
FM_OBJS      = $(FM_DIR)/nex-fm.o
SESSION_OBJS = $(SESSION_DIR)/nex-session.o

# ── Targets ───────────────────────────────────────────────────────────────────
TARGET         = $(BINDIR)/nexwm
CTL_TARGET     = $(BINDIR)/nexwmctl
PANEL_TARGET   = $(BINDIR)/nex-panel
LAUNCH_TARGET  = $(BINDIR)/nex-launcher
WALL_TARGET    = $(BINDIR)/nex-wallpaper
DESK_TARGET    = $(BINDIR)/nex-desktop
NOTIFY_TARGET  = $(BINDIR)/nex-notify
SETT_TARGET    = $(BINDIR)/nex-settings
FM_TARGET      = $(BINDIR)/nex-fm
SESSION_TARGET = $(BINDIR)/nex-session

ALL_COMPONENTS = $(PANEL_TARGET) $(LAUNCH_TARGET) $(WALL_TARGET) \
                 $(DESK_TARGET) $(NOTIFY_TARGET) $(SETT_TARGET) $(FM_TARGET) $(SESSION_TARGET)

.PHONY: all phase1 phase2 phase3 debug release clean install uninstall test test-hd test-display dirs wm ctl

all: dirs $(TARGET) $(CTL_TARGET) $(ALL_COMPONENTS)

phase1: dirs $(PANEL_TARGET) $(LAUNCH_TARGET) $(WALL_TARGET)
phase2: dirs $(DESK_TARGET) $(NOTIFY_TARGET)
phase3: dirs $(SETT_TARGET)

wm:  dirs $(TARGET)
ctl: dirs $(CTL_TARGET)

dirs:
	@mkdir -p $(BINDIR)

debug:
	$(MAKE) CFLAGS="$(DEBUG_CFLAGS)" LDFLAGS="$(DEBUG_LDFLAGS)" SAN_FLAGS="-fsanitize=address,undefined" all

release:
	$(MAKE) CFLAGS="$(CFLAGS) -DNDEBUG" all

# ── Link rules ────────────────────────────────────────────────────────────────
$(TARGET): $(WM_OBJS)
	$(CC) $(WM_OBJS) -o $@ $(LDFLAGS)

$(CTL_TARGET): $(CTL_OBJS)
	$(CC) $(CTL_OBJS) -o $@ $(SAN_FLAGS)

$(PANEL_TARGET): $(PANEL_OBJS)
	$(CC) $(PANEL_OBJS) -o $@ $(PANEL_LDFLAGS) $(SAN_FLAGS)

$(LAUNCH_TARGET): $(LAUNCH_OBJS)
	$(CXX) $(LAUNCH_OBJS) -o $@ $(QT6_LDFLAGS)

$(WALL_TARGET): $(WALL_OBJS)
	$(CC) $(WALL_OBJS) -o $@ $(WALLPAPER_LDFLAGS) $(SAN_FLAGS)

$(DESK_TARGET): $(DESK_OBJS)
	$(CC) $(DESK_OBJS) -o $@ $(DESKTOP_LDFLAGS) $(SAN_FLAGS)

$(NOTIFY_TARGET): $(NOTIFY_OBJS)
	$(CXX) $(NOTIFY_OBJS) -o $@ $(QT6_LDFLAGS)

$(SETT_TARGET): $(SETT_OBJS)
	$(CXX) $(SETT_OBJS) -o $@ $(QT6_LDFLAGS)

$(FM_TARGET): $(FM_OBJS)
	$(CXX) $(FM_OBJS) -o $@ $(QT6_LDFLAGS)
	ln -sf nex-fm $(BINDIR)/nex-filemanager

$(SESSION_TARGET): $(SESSION_OBJS)
	$(CXX) $(SESSION_OBJS) -o $@ $(QT6_LDFLAGS)

# ── Compile rules ─────────────────────────────────────────────────────────────
$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(PANEL_DIR)/%.o: $(PANEL_DIR)/%.c
	$(CC) $(CFLAGS) -I$(PANEL_DIR) -MMD -MP -c $< -o $@

$(LAUNCH_DIR)/%.o: $(LAUNCH_DIR)/%.cpp
	$(CXX) -std=c++17 $(QT6_CFLAGS) -I$(LAUNCH_DIR) -c $< -o $@

$(WALL_DIR)/%.o: $(WALL_DIR)/%.c
	$(CC) $(CFLAGS) $(WALLPAPER_CFLAGS) -I$(WALL_DIR) -MMD -MP -c $< -o $@

$(DESK_DIR)/%.o: $(DESK_DIR)/%.c
	$(CC) $(CFLAGS) -I$(DESK_DIR) -MMD -MP -c $< -o $@

$(NOTIFY_DIR)/%.o: $(NOTIFY_DIR)/%.cpp
	$(CXX) -std=c++17 $(QT6_CFLAGS) -I$(NOTIFY_DIR) -c $< -o $@

$(SETT_DIR)/%.o: $(SETT_DIR)/%.cpp
	$(CXX) -std=c++17 $(QT6_CFLAGS) -I$(SETT_DIR) -c $< -o $@

$(FM_DIR)/%.o: $(FM_DIR)/%.cpp
	$(CXX) -std=c++17 $(QT6_CFLAGS) -I$(FM_DIR) -c $< -o $@

$(SESSION_DIR)/%.o: $(SESSION_DIR)/%.cpp
	$(CXX) -std=c++17 $(QT6_CFLAGS) -I$(SESSION_DIR) -c $< -o $@

-include $(WM_DEPS)
-include $(CTL_DEPS)
-include $(PANEL_OBJS:.o=.d)
-include $(WALL_OBJS:.o=.d)
-include $(DESK_OBJS:.o=.d)

# ── Maintenance ───────────────────────────────────────────────────────────────
clean:
	rm -f $(SRCDIR)/*.o $(SRCDIR)/*.d
	rm -f $(PANEL_DIR)/*.o $(PANEL_DIR)/*.d
	rm -f $(LAUNCH_DIR)/*.o $(LAUNCH_DIR)/*.d
	rm -f $(WALL_DIR)/*.o $(WALL_DIR)/*.d
	rm -f $(DESK_DIR)/*.o $(DESK_DIR)/*.d
	rm -f $(NOTIFY_DIR)/*.o $(NOTIFY_DIR)/*.d
	rm -f $(SETT_DIR)/*.o $(SETT_DIR)/*.d
	rm -f $(FM_DIR)/*.o $(FM_DIR)/*.d
	rm -f $(SESSION_DIR)/*.o $(SESSION_DIR)/*.d
	rm -rf $(BINDIR)

install: all
	install -Dm755 $(TARGET)        $(BINDIR_INSTALL)/nexwm
	install -Dm755 $(CTL_TARGET)    $(BINDIR_INSTALL)/nexwmctl
	install -Dm755 $(PANEL_TARGET)  $(BINDIR_INSTALL)/nex-panel
	install -Dm755 $(LAUNCH_TARGET) $(BINDIR_INSTALL)/nex-launcher
	install -Dm755 $(WALL_TARGET)   $(BINDIR_INSTALL)/nex-wallpaper
	install -Dm755 $(DESK_TARGET)   $(BINDIR_INSTALL)/nex-desktop
	install -Dm755 $(NOTIFY_TARGET) $(BINDIR_INSTALL)/nex-notify
	install -Dm755 $(SETT_TARGET)   $(BINDIR_INSTALL)/nex-settings
	install -Dm755 $(FM_TARGET)     $(BINDIR_INSTALL)/nex-fm
	ln -sf nex-fm $(BINDIR_INSTALL)/nex-filemanager
	install -Dm755 $(SESSION_TARGET) $(BINDIR_INSTALL)/nex-session
	install -Dm755 config/start-nexde $(BINDIR_INSTALL)/start-nexde
	install -Dm644 config/nexwm.desktop /usr/share/xsessions/nexwm.desktop 2>/dev/null || true
	install -Dm644 config/nexwm.conf $(ETCDIR)/nexwm.conf
	install -Dm644 config/applications/nex-fm.desktop       $(PREFIX)/share/applications/nex-fm.desktop
	install -Dm644 config/applications/nex-settings.desktop $(PREFIX)/share/applications/nex-settings.desktop
	install -Dm644 config/applications/nex-terminal.desktop $(PREFIX)/share/applications/nex-terminal.desktop
	@echo "Nex Desktop Environment installed to $(PREFIX)"

uninstall:
	rm -f $(BINDIR_INSTALL)/nexwm
	rm -f $(BINDIR_INSTALL)/nexwmctl
	rm -f $(BINDIR_INSTALL)/nex-panel
	rm -f $(BINDIR_INSTALL)/nex-launcher
	rm -f $(BINDIR_INSTALL)/nex-wallpaper
	rm -f $(BINDIR_INSTALL)/nex-desktop
	rm -f $(BINDIR_INSTALL)/nex-notify
	rm -f $(BINDIR_INSTALL)/nex-settings
	rm -f $(BINDIR_INSTALL)/nex-fm
	rm -f $(BINDIR_INSTALL)/nex-filemanager
	rm -f $(BINDIR_INSTALL)/nex-session
	rm -f $(BINDIR_INSTALL)/start-nexde
	rm -f /usr/share/xsessions/nexwm.desktop
	rm -f $(PREFIX)/share/applications/nex-fm.desktop
	rm -f $(PREFIX)/share/applications/nex-settings.desktop
	rm -f $(PREFIX)/share/applications/nex-terminal.desktop
	rm -rf $(ETCDIR)

test: all
	@echo "Launching NexDE in Xephyr (1280x720 on :1) ..."
	@./scripts/test-nexde

test-hd: all
	@echo "Launching NexDE in Xephyr (1920x1080 on :1) ..."
	@./scripts/test-nexde 1920x1080

test-display: all
	@echo "Launching NexDE in Xephyr ($(SCREEN)) on $(XDISP) ..."
	@./scripts/test-nexde $(SCREEN) $(XDISP)

