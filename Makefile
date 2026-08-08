# NexWM Makefile
# Nex Window Manager & Desktop Suite

CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Wpedantic -std=c11 -O2 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDFLAGS ?= -lxcb -lxcb-util -lxcb-randr -lxcb-ewmh -lxcb-icccm -lxcb-keysyms -lX11

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

# ── Desktop Suite Components ──────────────────────────────────────────────────
PANEL_DIR    = components/panel
LAUNCH_DIR   = components/launcher
WALL_DIR     = components/wallpaper
DESK_DIR     = components/desktop
NOTIFY_DIR   = components/notify
SETT_DIR     = components/settings

PANEL_SRCS   = $(PANEL_DIR)/nex-panel.c
LAUNCH_SRCS  = $(LAUNCH_DIR)/nex-launcher.c
WALL_SRCS    = $(WALL_DIR)/nex-wallpaper.c
DESK_SRCS    = $(DESK_DIR)/nex-desktop.c
NOTIFY_SRCS  = $(NOTIFY_DIR)/nex-notify.c
SETT_SRCS    = $(SETT_DIR)/nex-settings.c

PANEL_OBJS   = $(PANEL_SRCS:.c=.o)
LAUNCH_OBJS  = $(LAUNCH_SRCS:.c=.o)
WALL_OBJS    = $(WALL_SRCS:.c=.o)
DESK_OBJS    = $(DESK_SRCS:.c=.o)
NOTIFY_OBJS  = $(NOTIFY_SRCS:.c=.o)
SETT_OBJS    = $(SETT_SRCS:.c=.o)

# ── Targets ───────────────────────────────────────────────────────────────────
TARGET         = $(BINDIR)/nexwm
CTL_TARGET     = $(BINDIR)/nexwmctl
PANEL_TARGET   = $(BINDIR)/nex-panel
LAUNCH_TARGET  = $(BINDIR)/nex-launcher
WALL_TARGET    = $(BINDIR)/nex-wallpaper
DESK_TARGET    = $(BINDIR)/nex-desktop
NOTIFY_TARGET  = $(BINDIR)/nex-notify
SETT_TARGET    = $(BINDIR)/nex-settings

ALL_COMPONENTS = $(PANEL_TARGET) $(LAUNCH_TARGET) $(WALL_TARGET) \
                 $(DESK_TARGET) $(NOTIFY_TARGET) $(SETT_TARGET)

.PHONY: all phase1 phase2 phase3 debug release clean install uninstall test dirs wm ctl

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
	$(CC) $(LAUNCH_OBJS) -o $@ $(SAN_FLAGS)

$(WALL_TARGET): $(WALL_OBJS)
	$(CC) $(WALL_OBJS) -o $@ $(WALLPAPER_LDFLAGS) $(SAN_FLAGS)

$(DESK_TARGET): $(DESK_OBJS)
	$(CC) $(DESK_OBJS) -o $@ $(DESKTOP_LDFLAGS) $(SAN_FLAGS)

$(NOTIFY_TARGET): $(NOTIFY_OBJS)
	$(CC) $(NOTIFY_OBJS) -o $@ $(NOTIFY_LDFLAGS) $(SAN_FLAGS)

$(SETT_TARGET): $(SETT_OBJS)
	$(CC) $(SETT_OBJS) -o $@ $(SAN_FLAGS)

# ── Compile rules ─────────────────────────────────────────────────────────────
$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(PANEL_DIR)/%.o: $(PANEL_DIR)/%.c
	$(CC) $(CFLAGS) -I$(PANEL_DIR) -MMD -MP -c $< -o $@

$(LAUNCH_DIR)/%.o: $(LAUNCH_DIR)/%.c
	$(CC) $(CFLAGS) -I$(LAUNCH_DIR) -MMD -MP -c $< -o $@

$(WALL_DIR)/%.o: $(WALL_DIR)/%.c
	$(CC) $(CFLAGS) $(WALLPAPER_CFLAGS) -I$(WALL_DIR) -MMD -MP -c $< -o $@

$(DESK_DIR)/%.o: $(DESK_DIR)/%.c
	$(CC) $(CFLAGS) -I$(DESK_DIR) -MMD -MP -c $< -o $@

$(NOTIFY_DIR)/%.o: $(NOTIFY_DIR)/%.c
	$(CC) $(CFLAGS) -I$(NOTIFY_DIR) -MMD -MP -c $< -o $@

$(SETT_DIR)/%.o: $(SETT_DIR)/%.c
	$(CC) $(CFLAGS) -I$(SETT_DIR) -MMD -MP -c $< -o $@

-include $(WM_DEPS)
-include $(CTL_DEPS)
-include $(PANEL_OBJS:.o=.d)
-include $(LAUNCH_OBJS:.o=.d)
-include $(WALL_OBJS:.o=.d)
-include $(DESK_OBJS:.o=.d)
-include $(NOTIFY_OBJS:.o=.d)
-include $(SETT_OBJS:.o=.d)

# ── Maintenance ───────────────────────────────────────────────────────────────
clean:
	rm -f $(SRCDIR)/*.o $(SRCDIR)/*.d
	rm -f $(PANEL_DIR)/*.o $(PANEL_DIR)/*.d
	rm -f $(LAUNCH_DIR)/*.o $(LAUNCH_DIR)/*.d
	rm -f $(WALL_DIR)/*.o $(WALL_DIR)/*.d
	rm -f $(DESK_DIR)/*.o $(DESK_DIR)/*.d
	rm -f $(NOTIFY_DIR)/*.o $(NOTIFY_DIR)/*.d
	rm -f $(SETT_DIR)/*.o $(SETT_DIR)/*.d
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
	install -Dm644 config/nexwm.conf $(ETCDIR)/nexwm.conf
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
	rm -rf $(ETCDIR)

test:
	@echo "=== Nex Desktop Environment test guide ==="
	@echo "1. Start Xephyr display:"
	@echo "   Xephyr :1 -ac -br -noreset -screen 1280x720 &"
	@echo ""
	@echo "2. Start NexWM:"
	@echo "   DISPLAY=:1 ./$(TARGET) --debug &"
	@echo ""
	@echo "3. Run Desktop components:"
	@echo "   DISPLAY=:1 ./$(PANEL_TARGET) &"
	@echo "   DISPLAY=:1 ./$(WALL_TARGET) --color 0x1a1a2e &"
	@echo "   DISPLAY=:1 ./$(DESK_TARGET) &"
	@echo "   DISPLAY=:1 ./$(NOTIFY_TARGET) \"NexDE\" \"Desktop ready\" 3000 &"
	@echo "   ./$(SETT_TARGET)"
