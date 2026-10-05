CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c11
CPPFLAGS += -Ihde-core/include -Isrc
BUILD ?= build
CORE_SRC=hde-core/integration/core.c hde-core/settings/settings.c hde-core/session/session.c hde-core/desktop/desktop.c hde-core/panel/panel.c hde-core/notifications/notifications.c
BACKEND_SRC=backend/backend.c backend/x11/x11_backend.c backend/wayland/wayland_backend.c
CORE_OBJ=$(CORE_SRC:.c=.o) $(BACKEND_SRC:.c=.o)

GTK_CFLAGS:=$(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
GTK_LIBS:=$(shell pkg-config --libs gtk+-3.0 2>/dev/null)
WNCK_CFLAGS:=$(shell pkg-config --cflags libwnck-3.0 x11 2>/dev/null)
WNCK_LIBS:=$(shell pkg-config --libs libwnck-3.0 x11 2>/dev/null)
GLIBX_CFLAGS:=$(shell pkg-config --cflags glib-2.0 x11 2>/dev/null)
GLIBX_LIBS:=$(shell pkg-config --libs glib-2.0 x11 2>/dev/null || echo "-lglib-2.0 -lX11")
# XInput2 (libxi-dev): needed for the Super key to open the Start menu. Without it hde-hotkeys still builds, just without Super.
XI_CFLAGS:=$(shell pkg-config --exists xi 2>/dev/null && echo "-DHAVE_XI2 `pkg-config --cflags xi`")
XI_LIBS:=$(shell pkg-config --libs xi 2>/dev/null)

# Flags for the GTK programs in src/
GUI_CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
GUI_CPPFLAGS = -Isrc -DWNCK_I_KNOW_THIS_IS_UNSTABLE

HDE_HEADERS=$(wildcard src/*.h)
PANEL_SRC=src/hde-panel.c src/hde-tray.c src/hde-status.c src/hde-osd.c src/hde-notify.c src/hde-search.c src/hde-theme.c
DESKTOP_SRC=src/hde-desktop.c src/hde-theme.c
SETTINGS_SRC=src/hde-settings.c src/hde-settings-network.c src/hde-settings-bluetooth.c \
             src/hde-settings-appearance.c src/hde-settings-windows.c src/hde-settings-keyboard.c \
             src/hde-settings-sound.c src/hde-theme.c

PROGRAMS=hde-session hde-desktop hde-panel hde-settings hde-hotkeys hde-xsettings hde-screenshot

all: $(BUILD)/hde-core-demo $(BUILD)/hde-session components

# The real desktop / panel / settings (GTK3) live in src/. They are built into build/ so that hde-session
# (which looks next to itself first) runs the new copies instead of falling back to old ones in /usr/local/bin.
components: $(BUILD)/hde-desktop $(BUILD)/hde-panel $(BUILD)/hde-settings $(BUILD)/hde-hotkeys $(BUILD)/hde-xsettings \
            $(BUILD)/hde-screenshot

$(BUILD)/hde-desktop: $(DESKTOP_SRC) $(HDE_HEADERS) | $(BUILD)
	$(CC) $(GUI_CFLAGS) $(GUI_CPPFLAGS) $(GTK_CFLAGS) -o $@ $(filter %.c,$^) $(GTK_LIBS) -lm
$(BUILD)/hde-panel: $(PANEL_SRC) $(HDE_HEADERS) | $(BUILD)
	$(CC) $(GUI_CFLAGS) $(GUI_CPPFLAGS) $(GTK_CFLAGS) $(WNCK_CFLAGS) -o $@ $(filter %.c,$^) $(GTK_LIBS) $(WNCK_LIBS) -lm
$(BUILD)/hde-settings: $(SETTINGS_SRC) $(HDE_HEADERS) | $(BUILD)
	$(CC) $(GUI_CFLAGS) $(GUI_CPPFLAGS) $(GTK_CFLAGS) -o $@ $(filter %.c,$^) $(GTK_LIBS) -lm
$(BUILD)/hde-hotkeys: src/hde-hotkeys.c src/hde-ipc.h src/hde-commands.h | $(BUILD)
	@[ -n "$(XI_CFLAGS)" ] || echo "WARNING: libxi-dev (pkg-config xi) not found: Super key will not open the Start menu"
	$(CC) -O2 -Wall -Wextra -std=c11 -Isrc $(XI_CFLAGS) -o $@ $< $(XI_LIBS) -lX11
# Built-in screenshot tool (PrtSc / Shift+PrtSc / Alt+PrtSc via hde-hotkeys): no scrot & co. needed
$(BUILD)/hde-screenshot: src/hde-screenshot.c | $(BUILD)
	$(CC) $(GUI_CFLAGS) $(GTK_CFLAGS) $(GLIBX_CFLAGS) -o $@ $< $(GTK_LIBS) $(GLIBX_LIBS) -lm
$(BUILD)/hde-xsettings: src/hde-xsettings.c | $(BUILD)
	$(CC) -O2 -Wall -Wextra -std=c11 $(GLIBX_CFLAGS) -o $@ $< $(GLIBX_LIBS)
$(BUILD):
	mkdir -p $(BUILD)
$(BUILD)/hde-core-demo: apps/hde-core-demo.c $(CORE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^
$(BUILD)/hde-session: apps/hde-session.c src/hde-wm.h $(CORE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(filter-out %.h,$^)
%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<
backend/x11/x11_backend.o: src/hde-commands.h

# Smoke test: runs a whole HDE session in Xvfb (needs xvfb, xdotool, dbus-x11). See tests/smoke.sh
check: all
	BUILD=$(BUILD) sh tests/smoke.sh

clean:
	rm -rf $(BUILD) $(CORE_OBJ)
PREFIX ?= /usr/local
XSESSIONS ?= /usr/share/xsessions
APPS_DIR ?= /usr/share/applications

install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(XSESSIONS) $(DESTDIR)$(APPS_DIR)
	install -m755 $(BUILD)/hde-session $(DESTDIR)$(PREFIX)/bin/hde-session
	for b in hde-desktop hde-panel hde-settings hde-hotkeys hde-xsettings hde-screenshot; do \
	  if [ -x $(BUILD)/$$b ]; then install -m755 $(BUILD)/$$b $(DESTDIR)$(PREFIX)/bin/$$b; \
	  else echo "WARNING: $(BUILD)/$$b missing (libgtk-3-dev / libwnck-3-dev / libxi-dev not installed?)"; fi; done
	install -m755 data/hde-start $(DESTDIR)$(PREFIX)/bin/hde-start
	sed 's|@PREFIX@|$(PREFIX)|g' data/hde.desktop > $(DESTDIR)$(XSESSIONS)/hde.desktop
	sed 's|@PREFIX@|$(PREFIX)|g' data/hyggshi-settings.desktop > $(DESTDIR)$(APPS_DIR)/hyggshi-settings.desktop
	sed 's|@PREFIX@|$(PREFIX)|g' data/hde-screenshot.desktop > $(DESTDIR)$(APPS_DIR)/hde-screenshot.desktop
	chmod 644 $(DESTDIR)$(XSESSIONS)/hde.desktop $(DESTDIR)$(APPS_DIR)/hyggshi-settings.desktop $(DESTDIR)$(APPS_DIR)/hde-screenshot.desktop
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
	rm -f $(DESTDIR)$(XSESSIONS)/hde.desktop $(DESTDIR)$(APPS_DIR)/hyggshi-settings.desktop $(DESTDIR)$(APPS_DIR)/hde-screenshot.desktop
.PHONY: all clean install uninstall components dev reload check
