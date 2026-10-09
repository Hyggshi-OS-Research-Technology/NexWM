CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c11
CPPFLAGS += -Ihde-core/include -Isrc
BUILD ?= build
.DEFAULT_GOAL := all
CORE_SRC=hde-core/integration/core.c hde-core/settings/settings.c hde-core/session/session.c hde-core/desktop/desktop.c hde-core/panel/panel.c hde-core/notifications/notifications.c
BACKEND_SRC=backend/backend.c backend/x11/x11_backend.c backend/wayland/wayland_backend.c
CORE_OBJ=$(CORE_SRC:.c=.o) $(BACKEND_SRC:.c=.o)

GTK_CFLAGS:=$(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
GTK_LIBS:=$(shell pkg-config --libs gtk+-3.0 2>/dev/null)
# HDE Cmd's optional VTE backend: hde-cmd/src/vt.c is still the default; --vte opts into this adapter only when VTE
# development files were available while building.
CMD_VTE_PC:=$(shell pkg-config --exists vte-2.91 2>/dev/null && echo vte-2.91)
CMD_VTE_CFLAGS:=$(shell [ -n "$(CMD_VTE_PC)" ] && echo "-DHDE_CMD_HAVE_VTE `pkg-config --cflags $(CMD_VTE_PC)`")
CMD_VTE_LIBS:=$(shell [ -n "$(CMD_VTE_PC)" ] && pkg-config --libs $(CMD_VTE_PC))
WNCK_CFLAGS:=$(shell pkg-config --cflags libwnck-3.0 x11 2>/dev/null)
WNCK_LIBS:=$(shell pkg-config --libs libwnck-3.0 x11 2>/dev/null)
GLIBX_CFLAGS:=$(shell pkg-config --cflags glib-2.0 x11 2>/dev/null)
GLIBX_LIBS:=$(shell pkg-config --libs glib-2.0 x11 2>/dev/null || echo "-lglib-2.0 -lX11")
# XInput2 (libxi-dev, pulled in by libgtk-3-dev): needed for the Super key to open the Start menu and for the touchpad /
# mouse settings (natural scrolling, tap to click). Without it everything still builds, just without those.
XI_CFLAGS:=$(shell pkg-config --exists xi 2>/dev/null && echo "-DHAVE_XI2 `pkg-config --cflags xi`")
XI_LIBS:=$(shell pkg-config --libs xi 2>/dev/null)
X11_LIBS:=$(shell pkg-config --libs x11 2>/dev/null || echo -lX11)
# XRandR (libxrandr-dev, pulled in by libgtk-3-dev): F8 / Super+P screen layouts (Project), screens plugged in or out,
# software brightness for F6/F7 on screens without a backlight, Night Light. Without it: none of these.
XRANDR_CFLAGS:=$(shell pkg-config --exists xrandr 2>/dev/null && echo "-DHAVE_XRANDR `pkg-config --cflags xrandr`")
XRANDR_LIBS:=$(shell pkg-config --libs xrandr 2>/dev/null)
# XFixes (libxfixes-dev, pulled in by libgtk-3-dev): the mouse pointer in screenshots (hde-screenshot --pointer)
XFIXES_CFLAGS:=$(shell pkg-config --exists xfixes 2>/dev/null && echo "-DHAVE_XFIXES `pkg-config --cflags xfixes`")
XFIXES_LIBS:=$(shell pkg-config --libs xfixes 2>/dev/null)
# XCB (libxcb1-dev, libxcb-devel): NexWM, the window manager of HDE, speaks to the X server through it directly — no
# libX11, no toolkit in between. Without it there is no `nexwm --x11` (the program says what to install).
XCB_CFLAGS:=$(shell pkg-config --exists xcb 2>/dev/null && echo "-DNEXWM_HAVE_XCB `pkg-config --cflags xcb`")
XCB_LIBS:=$(shell pkg-config --libs xcb 2>/dev/null)
# wlroots (libwlroots-dev): NexWM's Wayland compositor. 0.17 is the oldest API this source supports; the unversioned
# pkg-config name is tried last and its version is checked, so an older `wlroots.pc` cannot accidentally enable it.
WLR_PC:=$(shell for n in wlroots-0.21 wlroots-0.20 wlroots-0.19 wlroots-0.18 wlroots-0.17 wlroots; do \
                    pkg-config --exists $$n 2>/dev/null || continue; \
                    v=`pkg-config --modversion $$n`; major=`echo $$v | cut -d. -f1`; minor=`echo $$v | cut -d. -f2`; \
                    [ "$$major" = 0 ] && [ "$$minor" -ge 17 ] && { echo $$n; break; }; done)
WLR_MINOR:=$(shell [ -n "$(WLR_PC)" ] && pkg-config --modversion $(WLR_PC) | cut -d. -f2)
WLR_CFLAGS:=$(shell [ -n "$(WLR_PC)" ] && echo "-DNEXWM_HAVE_WLROOTS -DWLR_USE_UNSTABLE -DNEXWM_WLROOTS_MINOR=$(WLR_MINOR) `pkg-config --cflags $(WLR_PC)`")
WLR_LIBS:=$(shell [ -n "$(WLR_PC)" ] && pkg-config --libs $(WLR_PC) wayland-server xkbcommon)
# A small Wayland client verifies the real headless compositor in `make check-nexwm`; keep it optional alongside wlroots.
NEXWM_WAYLAND_PROBE:=$(shell [ -n "$(WLR_PC)" ] && pkg-config --exists wayland-client 2>/dev/null && echo yes)
ifeq ($(NEXWM_WAYLAND_PROBE),yes)
NEXWM_WAYLAND_PROBE_TARGET=$(BUILD)/nexwm-wayland-probe
NEXWM_WAYLAND_CLIENT_CFLAGS=$(shell pkg-config --cflags wayland-client)
NEXWM_WAYLAND_CLIENT_LIBS=$(shell pkg-config --libs wayland-client)
endif
# gtk-layer-shell (libgtk-layer-shell-dev): the "HDE (Wayland)" session — panel, desktop, Start menu and popups as
# layer-shell surfaces. Without it HDE builds for X11 only. It must come before libwayland-client when linking.
LAYER_CFLAGS:=$(shell pkg-config --exists gtk-layer-shell-0 2>/dev/null && echo "-DHAVE_GTK_LAYER_SHELL `pkg-config --cflags gtk-layer-shell-0`")
LAYER_LIBS:=$(shell pkg-config --exists gtk-layer-shell-0 2>/dev/null && pkg-config --libs gtk-layer-shell-0)
# wayland-scanner is used both for the panel's Wayland taskbar and for the xdg-shell header wlroots asks compositor
# consumers to generate. The latter comes from wayland-protocols (Debian: wayland-protocols; Fedora: wayland-protocols-devel).
WAYLAND_SCANNER:=$(shell pkg-config --variable=wayland_scanner wayland-scanner 2>/dev/null || command -v wayland-scanner 2>/dev/null)
WAYLAND_PROTOCOLS_DIR:=$(shell pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null)
NEXWM_XDG_SHELL_XML=$(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml
NEXWM_WLR_PROTOCOL_HEADER :=
ifneq ($(WLR_PC),)
NEXWM_WLR_PROTOCOL_HEADER := $(BUILD)/xdg-shell-protocol.h
$(NEXWM_WLR_PROTOCOL_HEADER): $(wildcard $(NEXWM_XDG_SHELL_XML)) | $(BUILD)
	@test -n "$(WAYLAND_SCANNER)" || { echo "ERROR: NexWM's wlroots compositor needs wayland-scanner (install libwayland-dev)" >&2; exit 1; }
	@test -f "$(NEXWM_XDG_SHELL_XML)" || { echo "ERROR: NexWM's wlroots compositor needs wayland-protocols (install the wayland-protocols package)" >&2; exit 1; }
	$(WAYLAND_SCANNER) server-header "$(NEXWM_XDG_SHELL_XML)" "$@"
endif
WLTASK:=$(shell [ -n "$(LAYER_CFLAGS)" ] && [ -n "$(WAYLAND_SCANNER)" ] && pkg-config --exists wayland-client 2>/dev/null && echo yes)
FTM=wlr-foreign-toplevel-management-unstable-v1
ifeq ($(WLTASK),yes)
WLTASK_CFLAGS=-DHAVE_WAYLAND_TASKBAR -I$(BUILD) $(shell pkg-config --cflags wayland-client)
WLTASK_LIBS=$(shell pkg-config --libs wayland-client)
WLTASK_SRC=src/hde-wltaskbar.c $(BUILD)/$(FTM)-protocol.c
WLTASK_DEPS=$(BUILD)/$(FTM)-client-protocol.h
endif

# hde-lock (src/hde-lock.c): HDE's own lock screen, on both of HDE's sessions — the "HDE" (X11) session with a
# full-screen window the window manager cannot touch (libxcb; no libX11, no toolkit needed) and the "HDE (Wayland)"
# session with the compositor's session lock (ext-session-lock-v1, which labwc and NexWM offer; the protocol is
# generated from protocols/ext-session-lock-v1.xml, byte-identical to wayland-protocols', the same way the panel's
# taskbar protocol is). The picture is drawn with cairo (the GTK programs already bring it in) and the password is
# checked through PAM (libpam0g-dev / pam-devel). A build without PAM still makes a hde-lock that says so and refuses
# to lock, so HDE_SH_LOCK (src/hde-commands.h) can fall back to the lockers the machine does have.
LOCKXML=ext-session-lock-v1
LOCK_CAIRO:=$(shell pkg-config --exists cairo 2>/dev/null && echo yes)
# PAM is asked for the way a compiler asks for it: the header and -lpam (libpam0g-dev on Debian/Ubuntu, pam-devel on
# Fedora). Not through pkg-config -- pam.pc is not in every one of those packages.
LOCK_PAM:=$(shell printf '#include <security/pam_appl.h>\nint main(void){return 0;}\n' | $(CC) -x c - -o /dev/null -lpam >/dev/null 2>&1 && echo yes)
LOCK_XCB:=$(shell pkg-config --exists xcb 2>/dev/null && echo yes)
LOCK_WL:=$(shell [ -n "$(WAYLAND_SCANNER)" ] && pkg-config --exists wayland-client xkbcommon 2>/dev/null && echo yes)
HDE_LOCK:=$(shell [ -n "$(LOCK_CAIRO)" ] && { [ -n "$(LOCK_XCB)" ] || [ -n "$(LOCK_WL)" ]; } && echo yes)
ifeq ($(HDE_LOCK),yes)
HDE_LOCK_TARGET=$(BUILD)/hde-lock
HDE_LOCK_CFLAGS=$(shell pkg-config --cflags cairo) $(if $(LOCK_XCB),-DHDE_LOCK_HAVE_XCB $(shell pkg-config --cflags xcb)) \
                 $(if $(LOCK_PAM),-DHDE_LOCK_HAVE_PAM)
HDE_LOCK_LIBS=$(shell pkg-config --libs cairo) $(if $(LOCK_XCB),$(shell pkg-config --libs xcb)) \
               $(if $(LOCK_PAM),-lpam) -lm
ifeq ($(LOCK_WL),yes)
HDE_LOCK_CFLAGS+=-DHDE_LOCK_HAVE_WAYLAND -I$(BUILD) $(shell pkg-config --cflags wayland-client xkbcommon)
HDE_LOCK_LIBS+=$(shell pkg-config --libs wayland-client xkbcommon)
HDE_LOCK_DEPS=$(BUILD)/$(LOCKXML)-client-protocol.h $(BUILD)/$(LOCKXML)-protocol.c
endif
endif

# Flags for the GTK programs in src/
GUI_CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
GUI_CPPFLAGS = -Isrc -DWNCK_I_KNOW_THIS_IS_UNSTABLE -DHDE_DATADIR=\"$(PREFIX)/share/hde\"

HDE_HEADERS=$(wildcard src/*.h)
PANEL_SRC=src/hde-panel.c src/hde-tray.c src/hde-status.c src/hde-osd.c src/hde-notify.c src/hde-search.c src/hde-theme.c \
          src/hde-input.c src/hde-startmenu.c src/hde-applets.c src/hde-panel-config.c src/hde-osinfo.c src/hde-svgpath.c \
          src/hde-wl.c src/hde-run.c src/hde-flyout.c src/hde-control.c src/hde-battery.c src/hde-power.c \
          src/hde-profiles.c src/hde-powersave.c src/hde-measure.c $(WLTASK_SRC)
DESKTOP_SRC=src/hde-desktop.c src/hde-theme.c src/hde-panel-config.c src/hde-wl.c
SETTINGS_SRC=src/hde-settings.c src/hde-settings-network.c src/hde-settings-bluetooth.c \
             src/hde-settings-appearance.c src/hde-settings-windows.c src/hde-settings-keyboard.c \
             src/hde-settings-sound.c src/hde-settings-touchpad.c src/hde-settings-display.c src/hde-settings-about.c \
             src/hde-theme.c src/hde-input.c src/hde-randr.c src/hde-brightness.c src/hde-sysinfo.c \
             src/hde-settings-panel.c src/hde-panel-config.c src/hde-osinfo.c src/hde-svgpath.c src/hde-wl.c \
             src/hde-settings-wayland.c src/hde-settings-power.c src/hde-power.c src/hde-profiles.c src/hde-run.c \
             src/hde-settings-peripherals.c src/hde-measure.c
# Hyggshi Files, the file manager (its own folder: hde-files/, which also has a Makefile to build it alone)
FILES_SRC=$(wildcard hde-files/src/*.c) src/hde-theme.c
# Hyggshi Media, the pictures (its own folder: hde-media/, which also has a Makefile to build it alone); the media
# player and the recorder join it there
MEDIA_SRC=$(wildcard hde-media/src/*.c) src/hde-theme.c
# HDE Cmd, the terminal emulator (hde-cmd/: its own GTK window and PTY, built-in VT engine, optional VTE adapter)
CMD_SRC=$(wildcard hde-cmd/src/*.c)
CMD_HEADERS=$(wildcard hde-cmd/src/*.h)
# Build stamp (commit + date) shown in Settings > About, by --version and at the top of the session log, to tell at a
# glance whether the programs that run are the ones just built. Rewritten only when it changes (then only the three
# programs that show it are rebuilt). See scripts/hde-version.sh.
VERSION_H = $(BUILD)/hde-version.h
# NexWM (nexwm/), the window manager of HDE: one program for both, told apart by --x11 (the window manager, XCB) and
# --wayland (the compositor, wlroots). nexwm/src/config.c is the part that needs neither of them (nexwm.conf).
NEXWM_SRC=$(wildcard nexwm/src/*.c)
NEXWM_HEADERS=$(wildcard nexwm/src/*.h)

PROGRAMS=hde-session hde-desktop hde-panel hde-settings hde-hotkeys hde-xsettings hde-screenshot hde-files hde-media \
         hde-choose hde-cmd nexwm hde-lock

all: $(BUILD)/hde-core-demo $(BUILD)/hde-session components

# The real desktop / panel / settings (GTK3) live in src/. They are built into build/ so that hde-session
# (which looks next to itself first) runs the new copies instead of falling back to old ones in /usr/local/bin.
components: $(BUILD)/hde-desktop $(BUILD)/hde-panel $(BUILD)/hde-settings $(BUILD)/hde-hotkeys $(BUILD)/hde-xsettings \
            $(BUILD)/hde-screenshot $(BUILD)/hde-files $(BUILD)/hde-media $(BUILD)/hde-choose $(BUILD)/hde-cmd $(BUILD)/nexwm \
            $(HDE_LOCK_TARGET)

$(BUILD)/hde-desktop: $(DESKTOP_SRC) $(HDE_HEADERS) | $(BUILD)
	$(CC) $(GUI_CFLAGS) $(GUI_CPPFLAGS) $(GTK_CFLAGS) $(LAYER_CFLAGS) -o $@ $(filter %.c,$^) $(LAYER_LIBS) $(GTK_LIBS) -lm
$(BUILD)/hde-panel: $(PANEL_SRC) $(HDE_HEADERS) $(WLTASK_DEPS) | $(BUILD)
	@[ -n "$(LAYER_CFLAGS)" ] || echo "NOTE: libgtk-layer-shell-dev (pkg-config gtk-layer-shell-0) not found: HDE for X11 only, no Wayland session"
	$(CC) $(GUI_CFLAGS) $(GUI_CPPFLAGS) $(GTK_CFLAGS) $(WNCK_CFLAGS) $(XI_CFLAGS) $(XRANDR_CFLAGS) $(LAYER_CFLAGS) \
	    $(WLTASK_CFLAGS) -o $@ $(filter %.c,$^) $(LAYER_LIBS) $(GTK_LIBS) $(WNCK_LIBS) $(XI_LIBS) $(XRANDR_LIBS) $(X11_LIBS) \
	    $(WLTASK_LIBS) -lm
$(BUILD)/$(FTM)-client-protocol.h: protocols/$(FTM).xml | $(BUILD)
	$(WAYLAND_SCANNER) client-header $< $@
$(BUILD)/$(FTM)-protocol.c: protocols/$(FTM).xml | $(BUILD)
	$(WAYLAND_SCANNER) private-code $< $@
$(BUILD)/$(LOCKXML)-client-protocol.h: protocols/$(LOCKXML).xml | $(BUILD)
	$(WAYLAND_SCANNER) client-header $< $@
$(BUILD)/$(LOCKXML)-protocol.c: protocols/$(LOCKXML).xml | $(BUILD)
	$(WAYLAND_SCANNER) private-code $< $@
$(BUILD)/hde-settings: $(SETTINGS_SRC) $(HDE_HEADERS) $(VERSION_H) | $(BUILD)
	@[ -n "$(XRANDR_CFLAGS)" ] || echo "WARNING: libxrandr-dev (pkg-config xrandr) not found: no F8 screen layouts, no software brightness"
	$(CC) $(GUI_CFLAGS) $(GUI_CPPFLAGS) -I$(BUILD) $(GTK_CFLAGS) $(XI_CFLAGS) $(XRANDR_CFLAGS) $(LAYER_CFLAGS) -o $@ \
	    $(filter %.c,$^) $(LAYER_LIBS) $(GTK_LIBS) $(XI_LIBS) $(XRANDR_LIBS) $(X11_LIBS) -lm
$(BUILD)/hde-hotkeys: src/hde-hotkeys.c src/hde-brightness.c src/hde-randr.c src/hde-ipc.h src/hde-commands.h \
                      src/hde-brightness.h src/hde-randr.h | $(BUILD)
	@[ -n "$(XI_CFLAGS)" ] || echo "WARNING: libxi-dev (pkg-config xi) not found: Super key will not open the Start menu"
	$(CC) -O2 -Wall -Wextra -std=c11 -Isrc $(XI_CFLAGS) $(XRANDR_CFLAGS) -o $@ $(filter %.c,$^) $(XI_LIBS) $(XRANDR_LIBS) \
	    -lX11 -lm
# Built-in screenshot tool (PrtSc / Shift+PrtSc / Alt+PrtSc via hde-hotkeys): no scrot & co. needed
$(BUILD)/hde-screenshot: src/hde-screenshot.c | $(BUILD)
	$(CC) $(GUI_CFLAGS) $(GTK_CFLAGS) $(GLIBX_CFLAGS) $(XFIXES_CFLAGS) -o $@ $< $(GTK_LIBS) $(GLIBX_LIBS) $(XFIXES_LIBS) -lm
# hde-choose: "which program for this?" — when more than one program on the machine can do the same thing (a
# terminal, a file manager, a picture viewer, a music player, a screenshot tool, a system monitor), HDE asks, runs the
# one the user picked and remembers the answer in settings.ini (choice_<feature>). The rules live in src/hde-choose.c
# so that tests/choose-test.c can check them without a display. Run by the key bindings, the Start menu and Settings.
$(BUILD)/hde-choose: apps/hde-choose.c src/hde-choose.c src/hde-choose.h src/hde-distro.h src/hde-build.h | $(BUILD)
	$(CC) $(GUI_CFLAGS) $(GUI_CPPFLAGS) -I$(BUILD) $(GTK_CFLAGS) -o $@ $(filter %.c,$^) $(GTK_LIBS) -lm
# HDE Cmd (hde-cmd/): its own GTK terminal window and PTY, with the built-in VT parser as the default and VTE as an
# optional backend (`--vte`); hde-cmd/src/vt.c is display-independent and is covered by `make check-unit`.
$(BUILD)/hde-cmd: $(CMD_SRC) $(CMD_HEADERS) | $(BUILD)
	@[ -n "$(GTK_CFLAGS)" ] || echo "WARNING: libgtk-3-dev (pkg-config gtk+-3.0) not found: hde-cmd needs GTK 3"
	$(CC) $(GUI_CFLAGS) -std=c11 -Ihde-cmd/src $(GTK_CFLAGS) $(CMD_VTE_CFLAGS) -o $@ $(filter %.c,$^) $(GTK_LIBS) $(CMD_VTE_LIBS) -lutil -lm
# Hyggshi Files (hde-files/): folders, tabs, search, trash, thumbnails, drag and drop; the default file manager of HDE
$(BUILD)/hde-files: $(FILES_SRC) hde-files/src/files.h src/hde-theme.h | $(BUILD)
	$(CC) $(GUI_CFLAGS) -Ihde-files/src -Isrc $(GTK_CFLAGS) -o $@ $(filter %.c,$^) $(GTK_LIBS) -lm
# Hyggshi Media (hde-media/): the pictures — zoom, rotate, one after the other, full screen; the default picture viewer
# of HDE — and the music and the video (the player window, hde-media/src/player.c, on the plain-C state of playlist.c and
# engine.c). What has nothing to do with a window is built and run without a display by `make check-unit`;
# tests/media-test.sh drives the picture window for real.
$(BUILD)/hde-media: $(MEDIA_SRC) hde-media/src/media.h hde-media/src/viewer.h hde-media/src/player.h hde-media/src/playlist.h \
                    hde-media/src/engine.h hde-media/src/mpris.h src/hde-theme.h $(VERSION_H) | $(BUILD)
	$(CC) $(GUI_CFLAGS) -Ihde-media/src -Isrc -I$(BUILD) $(GTK_CFLAGS) -o $@ $(filter %.c,$^) $(GTK_LIBS) -lm
# XSETTINGS (live theme / Dark mode) + touchpad and mouse settings (login, live, hotplug, changes by other programs)
$(BUILD)/hde-xsettings: src/hde-xsettings.c src/hde-input.c src/hde-randr.c src/hde-brightness.c src/hde-input.h \
                        src/hde-randr.h src/hde-brightness.h src/hde-build.h $(VERSION_H) | $(BUILD)
	@[ -n "$(XI_CFLAGS)" ] || echo "WARNING: libxi-dev (pkg-config xi) not found: touchpad/mouse settings will not be applied"
	$(CC) -O2 -Wall -Wextra -std=c11 -Isrc -I$(BUILD) $(GLIBX_CFLAGS) $(XI_CFLAGS) $(XRANDR_CFLAGS) -o $@ $(filter %.c,$^) \
	    $(GLIBX_LIBS) $(XI_LIBS) $(XRANDR_LIBS) -lm
# The screen layouts of F8 without an X server (tests/randr-plan-test.c): run by `make check`
$(BUILD)/randr-plan-test: tests/randr-plan-test.c src/hde-randr.c src/hde-randr.h | $(BUILD)
	$(CC) -O2 -Wall -Wextra -std=c11 -Isrc -o $@ $(filter %.c,$^) -lm
# The SVG path reader that draws the distribution logos of data/logos (tests/svgpath-test.c): run by `make check`
$(BUILD)/svgpath-test: tests/svgpath-test.c src/hde-svgpath.c src/hde-svgpath.h | $(BUILD)
	$(CC) -O2 -Wall -Wextra -std=c11 -Isrc -o $@ $(filter %.c,$^) -lm
# The batteries of src/hde-power.c with fake /sys/class/power_supply trees (tests/power-test.c): run by `make check`
$(BUILD)/power-test: tests/power-test.c src/hde-power.c src/hde-power.h | $(BUILD)
	$(CC) -O2 -Wall -Wextra -std=c11 -Isrc $(GLIBX_CFLAGS) -o $@ $(filter %.c,$^) $(GLIBX_LIBS) -lm
# The package manager and the package names of the system in front of the user (tests/distro-test.c, fake os-release
# files): `make check-unit` runs it everywhere, tests/fedora-test.sh checks Fedora with it
$(BUILD)/distro-test: tests/distro-test.c src/hde-distro.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -o $@ $<
# Hyggshi Media's own logic (hde-media/src/gallery.c): the list, the order, the zoom ladder, the slideshow clock
$(BUILD)/media-test: tests/media-test.c hde-media/src/gallery.c hde-media/src/media.h | $(BUILD)
	$(CC) -O2 -Wall -Wextra -Wpedantic -std=c11 -Ihde-media/src -o $@ $(filter %.c,$^) -lm
# Hyggshi Media's player (hde-media/src/playlist.c): what is played, the .m3u/.pls files other players write, the tags
# read from the files themselves, and the state of the playback — plain C, no display and no sound card needed
$(BUILD)/player-test: tests/player-test.c hde-media/src/playlist.c hde-media/src/gallery.c hde-media/src/engine.c hde-media/src/playlist.h hde-media/src/engine.h hde-media/src/media.h | $(BUILD)
	$(CC) -O2 -Wall -Wextra -Wpedantic -std=c11 -Ihde-media/src -o $@ $(filter %.c,$^) -lm
# HDE Cmd's built-in VT parser and screen model (hde-cmd/src/vt.c): no GTK or display server needed
$(BUILD)/cmd-vt-test: tests/cmd-vt-test.c hde-cmd/src/vt.c hde-cmd/src/vt.h | $(BUILD)
	$(CC) -O2 -Wall -Wextra -Wpedantic -std=c11 -Ihde-cmd/src -o $@ $(filter %.c,$^)
# The stand-in for GTK3 of tests/choose-run-test.sh (tests/choose-stub/): the question of hde-choose ("which program
# for this?") cannot be answered in CI — there is no display, and a user clicking a radio button is not a test — so the
# test brings a toolkit of its own that says what was clicked (HDE_CHOOSE_STUB=..., see tests/choose-stub/gtk.c). It is
# only ever built for that test: the programs of HDE are built against the real GTK, and nothing here is installed.
$(BUILD)/hde-choose-stub.o: apps/hde-choose.c | $(BUILD)
	$(CC) $(GUI_CFLAGS) -Isrc -I$(BUILD) -Itests/choose-stub -c -Dmain=hde_choose_main -o $@ $<
$(BUILD)/hde-choose-stub: src/hde-choose.c src/hde-choose.h tests/choose-stub/gtk.c tests/choose-stub/gtk/gtk.h $(BUILD)/hde-choose-stub.o | $(BUILD)
	$(CC) $(GUI_CFLAGS) -Isrc -I$(BUILD) -Itests/choose-stub -o $@ $(filter %.c,$^) $(BUILD)/hde-choose-stub.o
# "Which program for this?" (src/hde-choose.c): the table of features, what is installed on a PATH the test makes up,
# the deduplication (x-terminal-emulator is a symlink to one of the others) and the answers in settings.ini — plain C
$(BUILD)/choose-test: tests/choose-test.c src/hde-choose.c src/hde-choose.h | $(BUILD)
	$(CC) -O2 -Wall -Wextra -Wpedantic -std=c11 -Isrc -o $@ $(filter %.c,$^)
# Measuring the screen and the panel (src/hde-measure.c): the checks without an X server (tests/measure-test.c), run by
# `make check`
$(BUILD)/measure-test: tests/measure-test.c src/hde-measure.c src/hde-measure.h | $(BUILD)
	$(CC) $(GUI_CFLAGS) $(GUI_CPPFLAGS) $(GTK_CFLAGS) $(XRANDR_CFLAGS) -o $@ $(filter %.c,$^) $(GTK_LIBS) $(XRANDR_LIBS) \
	    $(X11_LIBS) -lm
# HDE's lock screen (src/hde-lock.c, src/hde-lock-core.c): the "HDE" (X11) session gets a full-screen window the window
# manager cannot touch (libxcb; no libX11, no toolkit) and the "HDE (Wayland)" session the compositor's own session lock
# (ext-session-lock-v1, which labwc and NexWM offer; protocols/ext-session-lock-v1.xml is byte-identical to
# wayland-protocols'). It is the first locker HDE_SH_LOCK (src/hde-commands.h) tries; where it cannot lock (no cairo, no
# session lock, no PAM to check the password) its say-so in the log sends HDE on to the lockers the machine does have.
# `hde-lock --check` says 0 when this build can lock the session in front of it. Not built unless the libraries are
# there: $(HDE_LOCK) above says whether they are.
$(BUILD)/hde-lock: src/hde-lock.c src/hde-lock-core.c src/hde-lock-core.h $(HDE_HEADERS) $(HDE_LOCK_DEPS) $(VERSION_H) | $(BUILD)
	@[ -n "$(LOCK_PAM)" ] || echo "NOTE: libpam0g-dev / pam-devel (security/pam_appl.h and -lpam) not found: hde-lock cannot check a password and will refuse to lock"
	$(CC) $(GUI_CFLAGS) $(GUI_CPPFLAGS) -I$(BUILD) -std=c11 $(HDE_LOCK_CFLAGS) -o $@ src/hde-lock.c src/hde-lock-core.c $(if $(LOCK_WL),$(BUILD)/$(LOCKXML)-protocol.c) $(HDE_LOCK_LIBS)
# What hde-lock does with the typing, the clock and the English it shows (tests/lock-core-test.c): no display, no PAM
# and no session to lock, so `make check-unit` runs it everywhere
$(BUILD)/lock-core-test: tests/lock-core-test.c src/hde-lock-core.c src/hde-lock-core.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -o $@ tests/lock-core-test.c src/hde-lock-core.c
# NexWM (nexwm/): the window manager of HDE, built into build/nexwm — the X11 side (nexwm/src/x11.c: a real window
# manager, EWMH/ICCCM, XCB alone) and the Wayland side (nexwm/src/wayland.c: the compositor, wlroots), told apart by
# --x11 and --wayland. Neither library is required to build it: a build without libxcb has no window manager inside, a
# build without wlroots no compositor, and `nexwm --help`/the log say which one this is. `make check-unit` runs the
# configuration and key binding tests without any display; tests/nexwm-test.sh drives the X11 side in Xvfb.
$(BUILD)/nexwm: $(NEXWM_SRC) $(NEXWM_HEADERS) $(VERSION_H) $(NEXWM_WLR_PROTOCOL_HEADER) | $(BUILD)
	@[ -n "$(XCB_CFLAGS)" ] || echo "NOTE: libxcb1-dev (pkg-config xcb) not found: this nexwm has no X11 window manager"
	@[ -n "$(WLR_PC)" ] || echo "NOTE: libwlroots-dev (pkg-config wlroots) not found: this nexwm has no Wayland compositor"
	$(CC) $(GUI_CFLAGS) -std=c11 $(XCB_CFLAGS) $(WLR_CFLAGS) -Inexwm/src -Isrc -I$(BUILD) -o $@ $(filter %.c,$^) \
	    $(XCB_LIBS) $(WLR_LIBS) -lm
# NexWM's configuration file (nexwm/src/config.c) and the arithmetic of its frames (nexwm/src/frame.c: the title bar,
# its buttons, the mouse): no display, no X server, no window manager, so `make check-unit` runs them anywhere
$(BUILD)/nexwm-test: tests/nexwm-test.c nexwm/src/config.c nexwm/src/frame.c nexwm/src/frame.h nexwm/src/nexwm.h | $(BUILD)
	$(CC) -O2 -Wall -Wextra -Wpedantic -std=c11 -Inexwm/src -o $@ $(filter %.c,$^)
ifeq ($(NEXWM_WAYLAND_PROBE),yes)
$(BUILD)/nexwm-wayland-probe: tests/nexwm-wayland-probe.c | $(BUILD)
	$(CC) -O2 -Wall -Wextra -Wpedantic -std=c11 $(NEXWM_WAYLAND_CLIENT_CFLAGS) -o $@ $< $(NEXWM_WAYLAND_CLIENT_LIBS)
endif
# The window the test of the window manager puts on the screen (tests/nexwm-client.c): a plain XCB client, so it only
# exists where libxcb does (both are needed by tests/nexwm-test.sh, which then drives a real NexWM in Xvfb)
ifneq ($(XCB_CFLAGS),)
$(BUILD)/nexwm-client: tests/nexwm-client.c | $(BUILD)
	$(CC) -O2 -Wall -Wextra -Wpedantic -std=c11 $(XCB_CFLAGS) -o $@ $< $(XCB_LIBS)
endif
$(BUILD):
	mkdir -p $(BUILD)
$(VERSION_H): FORCE | $(BUILD)
	@sh scripts/hde-version.sh $@
FORCE:
$(BUILD)/hde-core-demo: apps/hde-core-demo.c $(CORE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^
$(BUILD)/hde-session: apps/hde-session.c src/hde-wm.h src/hde-build.h $(VERSION_H) $(CORE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(BUILD) -o $@ $(filter-out %.h,$^)
%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<
backend/x11/x11_backend.o: src/hde-commands.h
backend/wayland/wayland_backend.o: src/hde-commands.h

# The unit tests: no X server, no window manager, no session — the screen layouts, the distribution logos, the
# batteries, the panel measurement and the package manager of the system. `make check-unit` runs them on their own
# (also inside a minimal Fedora, see tests/fedora-test.sh --base)
UNIT_TESTS=$(BUILD)/randr-plan-test $(BUILD)/svgpath-test $(BUILD)/power-test $(BUILD)/measure-test $(BUILD)/lock-core-test \
            $(BUILD)/distro-test $(BUILD)/media-test $(BUILD)/player-test $(BUILD)/choose-test $(BUILD)/nexwm-test $(BUILD)/cmd-vt-test \
            tests/choose-run-test.sh tests/sddm-test.sh tests/session-entry-test.sh tests/arch-test.sh
# build/hde-choose (the real GTK program) is not a prerequisite: a machine without libgtk-3-dev still runs every unit
# test that does not need it, and tests/choose-run-test.sh falls back to the stand-in for GTK3.
check-unit: $(UNIT_TESTS) $(BUILD)/hde-choose-stub
	@rc=0; for t in $(UNIT_TESTS); do echo "== $$t"; \
	  if [ -x "$$t" ]; then "$$t"; else sh "$$t"; fi; st=$$?; \
	  if [ $$st != 0 ]; then echo "FAIL: unit test $$t (exit $$st)"; rc=1; fi; done; 	 if [ $$rc = 0 ]; then echo "== all unit tests passed"; else echo "== SOME UNIT TESTS FAILED"; fi; exit $$rc

# The HDE login screen (login/sddm/hde): files, metadata, theme.conf, QML and the installer; with a display (and
# SDDM's greeter installed) it renders the theme for real. See tests/sddm-test.sh
check-login:
	sh tests/sddm-test.sh

# HDE on Arch (packaging/arch/deps.sh, pacman, Arch's package names). `make check-unit` runs it everywhere — off an
# Arch it checks the list in the repository and skips the machine-specific half; this target is the same test on a
# machine where the whole thing runs. See tests/arch-test.sh
check-arch:
	sh tests/arch-test.sh

# Hyggshi Media in a real X server (Xvfb + Metacity): the picture viewer (the keys, the pixels, full screen, a second
# hde-media handing its picture to the window that is open — tests/media-test.sh) and then the player (the list, the
# transport, mpv over its socket, the end of a track — tests/player-window-test.sh, with a stand-in for mpv)
check-media: $(BUILD)/hde-media $(BUILD)/hde-hotkeys
	sh tests/media-test.sh
	sh tests/player-window-test.sh

# NexWM in a real X server (Xvfb + a window of its own): the takeover, the frames, the key bindings, the workspaces,
# the work area a panel reserves with its struts, the way out and --replace. See tests/nexwm-test.sh
ifneq ($(XCB_CFLAGS),)
check-nexwm: $(BUILD)/nexwm $(BUILD)/nexwm-client $(NEXWM_WAYLAND_PROBE_TARGET)
	sh tests/nexwm-test.sh
	@if [ -n "$(NEXWM_WAYLAND_PROBE_TARGET)" ]; then sh tests/nexwm-wayland-test.sh; \
	  else echo "SKIP: nexwm-wayland: wlroots or wayland-client development files were not present at build time"; fi
else
check-nexwm: $(BUILD)/nexwm $(NEXWM_WAYLAND_PROBE_TARGET)
	@echo "NOTE: libxcb1-dev (pkg-config xcb) not found: the X11 window-manager test needs an X server and XCB build"
	@if [ -n "$(NEXWM_WAYLAND_PROBE_TARGET)" ]; then sh tests/nexwm-wayland-test.sh; \
	  else echo "SKIP: nexwm-wayland: wlroots or wayland-client development files were not present at build time"; fi
endif

# Smoke test: runs a whole HDE session in Xvfb (needs xvfb, xdotool, dbus-x11). See tests/smoke.sh
check: all check-unit
	BUILD=$(BUILD) sh tests/smoke.sh

clean:
	rm -rf $(BUILD) $(CORE_OBJ)
PREFIX ?= /usr/local
SDDM_THEMES ?= /usr/share/sddm/themes
SDDM_THEME ?= hde
XSESSIONS ?= /usr/share/xsessions
WLSESSIONS ?= /usr/share/wayland-sessions
PORTALS_DIR ?= /usr/share/xdg-desktop-portal
APPS_DIR ?= /usr/share/applications

install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(XSESSIONS) $(DESTDIR)$(APPS_DIR)
	install -m755 $(BUILD)/hde-session $(DESTDIR)$(PREFIX)/bin/hde-session
	for b in hde-desktop hde-panel hde-settings hde-hotkeys hde-xsettings hde-screenshot hde-files hde-media hde-choose hde-cmd; do \
	  if [ -x $(BUILD)/$$b ]; then install -m755 $(BUILD)/$$b $(DESTDIR)$(PREFIX)/bin/$$b; \
	  else echo "WARNING: $(BUILD)/$$b missing (libgtk-3-dev / libwnck-3-dev / libxi-dev not installed?)"; fi; done
	install -m755 data/hde-start $(DESTDIR)$(PREFIX)/bin/hde-start
	@if [ -x $(BUILD)/hde-lock ]; then install -m755 $(BUILD)/hde-lock $(DESTDIR)$(PREFIX)/bin/hde-lock; \
	 else echo "NOTE: hde-lock was not built (needs pkg-config cairo and libxcb1-dev, or wayland-client + xkbcommon, and libpam0g-dev for the password check)"; fi
	install -m755 $(BUILD)/nexwm $(DESTDIR)$(PREFIX)/bin/nexwm
	install -d $(DESTDIR)$(PREFIX)/share/hde/logos $(DESTDIR)$(WLSESSIONS) $(DESTDIR)$(PORTALS_DIR)
	install -m644 data/logos/*.svg data/logos/LICENSES.md $(DESTDIR)$(PREFIX)/share/hde/logos/
	sed 's|@PREFIX@|$(PREFIX)|g' data/hde-wayland.desktop > $(DESTDIR)$(WLSESSIONS)/hde-wayland.desktop
	# "NexWM (Wayland)": HDE on HDE's own compositor; hde-session starts labwc instead on the machines where this
	# nexwm has no compositor yet, so the entry works everywhere (labwc stays the compositor of "HDE (Wayland)")
	sed 's|@PREFIX@|$(PREFIX)|g' data/nexwm-wayland.desktop > $(DESTDIR)$(WLSESSIONS)/nexwm-wayland.desktop
	chmod 644 $(DESTDIR)$(WLSESSIONS)/hde-wayland.desktop $(DESTDIR)$(WLSESSIONS)/nexwm-wayland.desktop
	install -m644 data/hde-portals.conf $(DESTDIR)$(PORTALS_DIR)/hde-portals.conf
	@command -v labwc >/dev/null 2>&1 || echo "NOTE: the HDE (Wayland) session appears on the login screen once labwc is installed (sudo apt install labwc)"
	sed 's|@PREFIX@|$(PREFIX)|g' data/hde.desktop > $(DESTDIR)$(XSESSIONS)/hde.desktop
	# "NexWM" on the login screen: the HDE session with NexWM as its window manager (hde-start --wm nexwm)
	sed 's|@PREFIX@|$(PREFIX)|g' data/nexwm.desktop > $(DESTDIR)$(XSESSIONS)/nexwm.desktop
	sed 's|@PREFIX@|$(PREFIX)|g' data/hyggshi-settings.desktop > $(DESTDIR)$(APPS_DIR)/hyggshi-settings.desktop
	sed 's|@PREFIX@|$(PREFIX)|g' data/hde-screenshot.desktop > $(DESTDIR)$(APPS_DIR)/hde-screenshot.desktop
	sed 's|@PREFIX@|$(PREFIX)|g' hde-files/hde-files.desktop > $(DESTDIR)$(APPS_DIR)/hde-files.desktop
	sed 's|@PREFIX@|$(PREFIX)|g' hde-media/hde-media.desktop > $(DESTDIR)$(APPS_DIR)/hde-media.desktop
	sed 's|@PREFIX@|$(PREFIX)|g' hde-cmd/hde-cmd.desktop > $(DESTDIR)$(APPS_DIR)/hde-cmd.desktop
	install -m644 hde-files/hde-mimeapps.list $(DESTDIR)$(APPS_DIR)/hde-mimeapps.list
	chmod 644 $(DESTDIR)$(XSESSIONS)/hde.desktop $(DESTDIR)$(XSESSIONS)/nexwm.desktop \
	    $(DESTDIR)$(APPS_DIR)/hyggshi-settings.desktop $(DESTDIR)$(APPS_DIR)/hde-screenshot.desktop \
	    $(DESTDIR)$(APPS_DIR)/hde-files.desktop $(DESTDIR)$(APPS_DIR)/hde-media.desktop \
	    $(DESTDIR)$(APPS_DIR)/hde-cmd.desktop
	-update-desktop-database $(DESTDIR)$(APPS_DIR) 2>/dev/null
	# the HDE login screen (SDDM theme, login/sddm/): the files, and hde-login to install/choose it on a running
	# system (it also writes /etc/sddm.conf.d/50-hde-theme.conf so SDDM uses the theme)
	install -d $(DESTDIR)$(SDDM_THEMES)/$(SDDM_THEME) $(DESTDIR)$(PREFIX)/bin
	install -m644 login/sddm/$(SDDM_THEME)/theme.conf login/sddm/$(SDDM_THEME)/metadata.desktop login/sddm/$(SDDM_THEME)/Main.qml $(DESTDIR)$(SDDM_THEMES)/$(SDDM_THEME)/
	cp -r login/sddm/$(SDDM_THEME)/components $(DESTDIR)$(SDDM_THEMES)/$(SDDM_THEME)/components
	cp -r login/sddm/$(SDDM_THEME)/assets $(DESTDIR)$(SDDM_THEMES)/$(SDDM_THEME)/assets
	chmod -R a+rX $(DESTDIR)$(SDDM_THEMES)/$(SDDM_THEME)
	install -m755 login/sddm/install.sh $(DESTDIR)$(PREFIX)/bin/hde-login
	@command -v sddm >/dev/null 2>&1 || echo "NOTE: the HDE login screen needs SDDM (sudo apt install sddm / sudo dnf install sddm), then: sudo hde-login"
# To see a new build WITHOUT LOGGING OUT:
#   make dev                     restart desktop+panel+hotkeys+xsettings from ./build (no install needed)
#   sudo make install && make reload   install into $(PREFIX), then ask hde-session to restart desktop+panel
dev: all
	-pkill -u $$(id -u) -x hde-panel
	-pkill -u $$(id -u) -x hde-desktop
	-pkill -u $$(id -u) -x hde-hotkeys
	-pkill -u $$(id -u) -x hde-xsettings
	sleep 1
	setsid -f $(BUILD)/hde-xsettings
	setsid -f $(BUILD)/hde-hotkeys
	setsid -f $(BUILD)/hde-desktop
	sleep 0.4
	setsid -f $(BUILD)/hde-panel
reload:
	$(PREFIX)/bin/hde-session restart
uninstall:
	for b in $(PROGRAMS) hde-start; do rm -f $(DESTDIR)$(PREFIX)/bin/$$b; done
	rm -f $(DESTDIR)$(XSESSIONS)/hde.desktop $(DESTDIR)$(XSESSIONS)/nexwm.desktop \
	      $(DESTDIR)$(APPS_DIR)/hyggshi-settings.desktop $(DESTDIR)$(APPS_DIR)/hde-screenshot.desktop
	rm -f $(DESTDIR)$(APPS_DIR)/hde-files.desktop $(DESTDIR)$(APPS_DIR)/hde-media.desktop \
	      $(DESTDIR)$(APPS_DIR)/hde-cmd.desktop $(DESTDIR)$(APPS_DIR)/hde-mimeapps.list
	rm -rf $(DESTDIR)$(PREFIX)/share/hde
	rm -rf $(DESTDIR)$(SDDM_THEMES)/$(SDDM_THEME)
	rm -f $(DESTDIR)$(PREFIX)/bin/hde-login
	rm -f $(DESTDIR)$(WLSESSIONS)/hde-wayland.desktop $(DESTDIR)$(WLSESSIONS)/nexwm-wayland.desktop \
	    $(DESTDIR)$(PORTALS_DIR)/hde-portals.conf
.PHONY: all clean install uninstall components dev reload check check-unit check-login check-arch check-media check-nexwm FORCE
