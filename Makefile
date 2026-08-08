# NexWM Makefile
# Nex Window Manager & Desktop Suite

CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Wpedantic -std=c11 -O2 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDFLAGS ?= -lxcb -lxcb-util -lxcb-randr -lxcb-ewmh -lxcb-icccm -lxcb-keysyms -lX11

# Panel uses xcb-ewmh for atom support
PANEL_LDFLAGS = -lxcb -lxcb-ewmh -lX11

# Wallpaper: XLib only (Imlib2 optional via HAVE_IMLIB2=1)
ifeq ($(HAVE_IMLIB2),1)
WALLPAPER_CFLAGS  = -DHAVE_IMLIB2
WALLPAPER_LDFLAGS = -lX11 -lImlib2
else
WALLPAPER_LDFLAGS = -lX11
endif

DEBUG_CFLAGS = -Wall -Wextra -Wpedantic -std=c11 -g -O0 -DDEBUG \
               -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
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

# ── Phase 1 Components ────────────────────────────────────────────────────────
PANEL_DIR    = components/panel
LAUNCH_DIR   = components/launcher
WALL_DIR     = components/wallpaper

PANEL_SRCS   = $(PANEL_DIR)/nex-panel.c
LAUNCH_SRCS  = $(LAUNCH_DIR)/nex-launcher.c
WALL_SRCS    = $(WALL_DIR)/nex-wallpaper.c

PANEL_OBJS   = $(PANEL_SRCS:.c=.o)
LAUNCH_OBJS  = $(LAUNCH_SRCS:.c=.o)
WALL_OBJS    = $(WALL_SRCS:.c=.o)

# ── Targets ───────────────────────────────────────────────────────────────────
TARGET         = $(BINDIR)/nexwm
CTL_TARGET     = $(BINDIR)/nexwmctl
PANEL_TARGET   = $(BINDIR)/nex-panel
LAUNCH_TARGET  = $(BINDIR)/nex-launcher
WALL_TARGET    = $(BINDIR)/nex-wallpaper

PHASE1_TARGETS = $(PANEL_TARGET) $(LAUNCH_TARGET) $(WALL_TARGET)

.PHONY: all phase1 debug release clean install uninstall test dirs wm ctl

all: dirs $(TARGET) $(CTL_TARGET) phase1

phase1: dirs $(PHASE1_TARGETS)

wm:  dirs $(TARGET)
ctl: dirs $(CTL_TARGET)

dirs:
	@mkdir -p $(BINDIR)

debug:
	$(MAKE) CFLAGS="$(DEBUG_CFLAGS)" LDFLAGS="$(DEBUG_LDFLAGS)" all

release:
	$(MAKE) CFLAGS="$(CFLAGS) -DNDEBUG" all

# ── NexWM & nexwmctl link rules ───────────────────────────────────────────────
$(TARGET): $(WM_OBJS)
	$(CC) $(WM_OBJS) -o $@ $(LDFLAGS)

$(CTL_TARGET): $(CTL_OBJS)
	$(CC) $(CTL_OBJS) -o $@

# ── Component link rules ──────────────────────────────────────────────────────
$(PANEL_TARGET): $(PANEL_OBJS)
	$(CC) $(PANEL_OBJS) -o $@ $(PANEL_LDFLAGS)

$(LAUNCH_TARGET): $(LAUNCH_OBJS)
	$(CC) $(LAUNCH_OBJS) -o $@

$(WALL_TARGET): $(WALL_OBJS)
	$(CC) $(WALL_OBJS) -o $@ $(WALLPAPER_LDFLAGS)

# ── Compile rules ─────────────────────────────────────────────────────────────
$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(PANEL_DIR)/%.o: $(PANEL_DIR)/%.c
	$(CC) $(CFLAGS) -I$(PANEL_DIR) -MMD -MP -c $< -o $@

$(LAUNCH_DIR)/%.o: $(LAUNCH_DIR)/%.c
	$(CC) $(CFLAGS) -I$(LAUNCH_DIR) -MMD -MP -c $< -o $@

$(WALL_DIR)/%.o: $(WALL_DIR)/%.c
	$(CC) $(CFLAGS) $(WALLPAPER_CFLAGS) -I$(WALL_DIR) -MMD -MP -c $< -o $@

-include $(WM_DEPS)
-include $(CTL_DEPS)
-include $(PANEL_OBJS:.o=.d)
-include $(LAUNCH_OBJS:.o=.d)
-include $(WALL_OBJS:.o=.d)

# ── Maintenance ───────────────────────────────────────────────────────────────
clean:
	rm -f $(SRCDIR)/*.o $(SRCDIR)/*.d
	rm -f $(PANEL_DIR)/*.o $(PANEL_DIR)/*.d
	rm -f $(LAUNCH_DIR)/*.o $(LAUNCH_DIR)/*.d
	rm -f $(WALL_DIR)/*.o $(WALL_DIR)/*.d
	rm -rf $(BINDIR)

install: all
	install -Dm755 $(TARGET)        $(BINDIR_INSTALL)/nexwm
	install -Dm755 $(CTL_TARGET)    $(BINDIR_INSTALL)/nexwmctl
	install -Dm755 $(PANEL_TARGET)  $(BINDIR_INSTALL)/nex-panel
	install -Dm755 $(LAUNCH_TARGET) $(BINDIR_INSTALL)/nex-launcher
	install -Dm755 $(WALL_TARGET)   $(BINDIR_INSTALL)/nex-wallpaper
	install -Dm644 config/nexwm.conf $(ETCDIR)/nexwm.conf
	@echo "Nex Desktop Environment installed to $(PREFIX)"

uninstall:
	rm -f $(BINDIR_INSTALL)/nexwm
	rm -f $(BINDIR_INSTALL)/nexwmctl
	rm -f $(BINDIR_INSTALL)/nex-panel
	rm -f $(BINDIR_INSTALL)/nex-launcher
	rm -f $(BINDIR_INSTALL)/nex-wallpaper
	rm -rf $(ETCDIR)

test:
	@echo "=== NexDE test guide ==="
	@echo "Start Xephyr:"
	@echo "  Xephyr :1 -ac -br -noreset -screen 1280x720 &"
	@echo ""
	@echo "Launch NexWM:"
	@echo "  DISPLAY=:1 ./$(TARGET) --debug &"
	@echo ""
	@echo "Launch panel, launcher, wallpaper:"
	@echo "  DISPLAY=:1 ./$(PANEL_TARGET) &"
	@echo "  DISPLAY=:1 ./$(WALL_TARGET) --color 0x1a1a2e"
	@echo "  DISPLAY=:1 ./$(LAUNCH_TARGET) --list"
	@echo ""
	@echo "Test IPC:"
	@echo "  DISPLAY=:1 ./$(CTL_TARGET) reload"
	@echo "  DISPLAY=:1 ./$(CTL_TARGET) workspace 2"
	@echo "  DISPLAY=:1 ./$(CTL_TARGET) set-gap 12"
