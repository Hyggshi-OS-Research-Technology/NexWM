CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
PREFIX  ?= /usr/local
BUILD   := build

GTK_FLAGS  := $(shell pkg-config --cflags --libs gtk+-3.0)
WNCK_FLAGS := $(shell pkg-config --cflags --libs gtk+-3.0 libwnck-3.0 x11 gio-unix-2.0)
GIO_FLAGS  := $(shell pkg-config --cflags --libs gio-unix-2.0)

all: $(BUILD)/hde-session $(BUILD)/hde-panel $(BUILD)/hde-desktop

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/hde-session: src/hde-session.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(GIO_FLAGS)

$(BUILD)/hde-panel: src/hde-panel.c src/hde-tray.c src/hde-tray.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/hde-panel.c src/hde-tray.c $(WNCK_FLAGS)

$(BUILD)/hde-desktop: src/hde-desktop.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(GTK_FLAGS)

install: all
	install -Dm755 $(BUILD)/hde-session $(DESTDIR)$(PREFIX)/bin/hde-session
	install -Dm755 $(BUILD)/hde-panel   $(DESTDIR)$(PREFIX)/bin/hde-panel
	install -Dm755 $(BUILD)/hde-desktop $(DESTDIR)$(PREFIX)/bin/hde-desktop
	install -Dm755 data/hde-start       $(DESTDIR)$(PREFIX)/bin/hde-start
	sed 's|@PREFIX@|$(PREFIX)|' data/hde.desktop > $(BUILD)/hde.desktop
	install -Dm644 $(BUILD)/hde.desktop $(DESTDIR)/usr/share/xsessions/hde.desktop

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/hde-session $(DESTDIR)$(PREFIX)/bin/hde-panel \
	      $(DESTDIR)$(PREFIX)/bin/hde-desktop $(DESTDIR)$(PREFIX)/bin/hde-start \
	      $(DESTDIR)/usr/share/xsessions/hde.desktop

clean:
	rm -rf $(BUILD)

.PHONY: all install uninstall clean
