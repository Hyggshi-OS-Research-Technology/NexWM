# Hyggshi Media (hde-media): the pictures of HDE — and, in this same folder, the music and the video that come next.
# The top-level Makefile builds and installs it too (make && sudo make install at the top); this one builds it alone:
#   make              ./hde-media           (needs libgtk-3-dev)
#   sudo make install $(PREFIX)/bin/hde-media and its menu entry
CC ?= cc
PREFIX ?= /usr/local
APPS_DIR ?= /usr/share/applications
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS := $(shell pkg-config --libs gtk+-3.0)
# hde-theme.c: Light / Dark and the accent colour of HDE's Settings
SRC = $(wildcard src/*.c) ../src/hde-theme.c

hde-media: $(SRC) src/media.h src/viewer.h ../src/hde-theme.h
	$(CC) $(CFLAGS) -Isrc -I../src $(GTK_CFLAGS) -o $@ $(filter %.c,$^) $(GTK_LIBS) -lm

# The menu entry and the default viewer for pictures in HDE sessions. hde-mimeapps.list (the list of HDE's default
# applications: it says image/* opens here) lives in hde-files/ and is installed by the top-level Makefile: one list for
# the whole desktop, one file to keep up to date.
install: hde-media
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(APPS_DIR)
	install -m755 hde-media $(DESTDIR)$(PREFIX)/bin/hde-media
	sed 's|@PREFIX@|$(PREFIX)|g' hde-media.desktop > $(DESTDIR)$(APPS_DIR)/hde-media.desktop
	chmod 644 $(DESTDIR)$(APPS_DIR)/hde-media.desktop
	-update-desktop-database $(DESTDIR)$(APPS_DIR) 2>/dev/null

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/hde-media $(DESTDIR)$(APPS_DIR)/hde-media.desktop

clean:
	rm -f hde-media

.PHONY: install uninstall clean
