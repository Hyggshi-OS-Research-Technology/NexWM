CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c11
CPPFLAGS += -Ihde-core/include
BUILD=build
CORE_SRC=hde-core/integration/core.c hde-core/settings/settings.c hde-core/session/session.c hde-core/desktop/desktop.c hde-core/panel/panel.c hde-core/notifications/notifications.c
BACKEND_SRC=backend/backend.c backend/x11/x11_backend.c backend/wayland/wayland_backend.c
CORE_OBJ=$(CORE_SRC:.c=.o) $(BACKEND_SRC:.c=.o)

GTK_CFLAGS:=$(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
GTK_LIBS:=$(shell pkg-config --libs gtk+-3.0 2>/dev/null)
WNCK_CFLAGS:=$(shell pkg-config --cflags libwnck-3.0 x11 2>/dev/null)
WNCK_LIBS:=$(shell pkg-config --libs libwnck-3.0 x11 2>/dev/null)

all: $(BUILD)/hde-core-demo $(BUILD)/hde-session $(BUILD)/hde-hotkeys components

# Desktop / panel / settings thật (GTK3) nằm trong src/. Build vào build/ để hde-session
# (tìm cạnh chính nó trước) dùng đúng bản mới, không rơi về bản cũ trong /usr/local/bin.
components: $(BUILD)/hde-desktop $(BUILD)/hde-panel $(BUILD)/hde-settings
$(BUILD)/hde-desktop: src/hde-desktop.c | $(BUILD)
	$(CC) -O2 -Wall $(GTK_CFLAGS) -o $@ $< $(GTK_LIBS) -lm
$(BUILD)/hde-panel: src/hde-panel.c src/hde-tray.c src/hde-tray.h | $(BUILD)
	$(CC) -O2 -Wall $(GTK_CFLAGS) $(WNCK_CFLAGS) -o $@ $(filter %.c,$^) $(GTK_LIBS) $(WNCK_LIBS) -lm
$(BUILD)/hde-settings: src/hde-settings.c | $(BUILD)
	$(CC) -O2 -Wall $(GTK_CFLAGS) -o $@ $< $(GTK_LIBS) -lm
$(BUILD)/hde-hotkeys: src/hde-hotkeys.c | $(BUILD)
	$(CC) -O2 -Wall -std=c11 -o $@ $< -lX11
$(BUILD):
	mkdir -p $(BUILD)
$(BUILD)/hde-core-demo: apps/hde-core-demo.c $(CORE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^
$(BUILD)/hde-session: apps/hde-session.c $(CORE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^
%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<
clean:
	rm -rf $(BUILD) $(CORE_OBJ)
PREFIX ?= /usr/local
XSESSIONS ?= /usr/share/xsessions
APPS_DIR ?= /usr/share/applications

install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(XSESSIONS) $(DESTDIR)$(APPS_DIR)
	install -m755 $(BUILD)/hde-session $(DESTDIR)$(PREFIX)/bin/hde-session
	for b in hde-desktop hde-panel hde-settings hde-hotkeys; do \
	  if [ -x $(BUILD)/$$b ]; then install -m755 $(BUILD)/$$b $(DESTDIR)$(PREFIX)/bin/$$b; \
	  else echo "WARNING: $(BUILD)/$$b missing (thiếu libgtk-3-dev / libwnck-3-dev?)"; fi; done
	install -m755 data/hde-start $(DESTDIR)$(PREFIX)/bin/hde-start
	sed 's|@PREFIX@|$(PREFIX)|g' data/hde.desktop > $(DESTDIR)$(XSESSIONS)/hde.desktop
	sed 's|@PREFIX@|$(PREFIX)|g' data/hyggshi-settings.desktop > $(DESTDIR)$(APPS_DIR)/hyggshi-settings.desktop
	chmod 644 $(DESTDIR)$(XSESSIONS)/hde.desktop $(DESTDIR)$(APPS_DIR)/hyggshi-settings.desktop
# Build xong muốn thấy bản mới MÀ KHÔNG PHẢI LOGOUT:
#   make dev                     chạy lại desktop+panel từ ./build (không cần cài)
#   sudo make install && make reload   cài vào $(PREFIX) rồi bảo hde-session chạy lại desktop+panel
dev: all
	-pkill -u $$(id -u) -x hde-panel
	-pkill -u $$(id -u) -x hde-desktop
	sleep 1
	setsid -f $(BUILD)/hde-desktop
	sleep 0.4
	setsid -f $(BUILD)/hde-panel
reload:
	$(PREFIX)/bin/hde-session restart
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/hde-session $(DESTDIR)$(PREFIX)/bin/hde-desktop \
	      $(DESTDIR)$(PREFIX)/bin/hde-panel $(DESTDIR)$(PREFIX)/bin/hde-settings $(DESTDIR)$(PREFIX)/bin/hde-hotkeys \
	      $(DESTDIR)$(PREFIX)/bin/hde-start $(DESTDIR)$(XSESSIONS)/hde.desktop \
	      $(DESTDIR)$(APPS_DIR)/hyggshi-settings.desktop
.PHONY: all clean install uninstall components dev reload
