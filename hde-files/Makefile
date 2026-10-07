# Hyggshi Files (hde-files): the file manager of HDE. The top-level Makefile builds and installs it too
# (make && sudo make install at the top); this one builds it alone:
#   make              ./hde-files            (needs libgtk-3-dev)
#   sudo make install $(PREFIX)/bin/hde-files, its menu entry, folders opening in it in HDE sessions
CC ?= cc
PREFIX ?= /usr/local
APPS_DIR ?= /usr/share/applications
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS := $(shell pkg-config --libs gtk+-3.0)
# hde-theme.c: Light / Dark and the accent colour of HDE's Settings
SRC = $(wildcard src/*.c) ../src/hde-theme.c

hde-files: $(SRC) src/files.h ../src/hde-theme.h
	$(CC) $(CFLAGS) -Isrc -I../src $(GTK_CFLAGS) -o $@ $(filter %.c,$^) $(GTK_LIBS)

install: hde-files
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(APPS_DIR)
	install -m755 hde-files $(DESTDIR)$(PREFIX)/bin/hde-files
	sed 's|@PREFIX@|$(PREFIX)|g' hde-files.desktop > $(DESTDIR)$(APPS_DIR)/hde-files.desktop
	install -m644 hde-mimeapps.list $(DESTDIR)$(APPS_DIR)/hde-mimeapps.list
	chmod 644 $(DESTDIR)$(APPS_DIR)/hde-files.desktop
	-update-desktop-database $(DESTDIR)$(APPS_DIR) 2>/dev/null

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/hde-files $(DESTDIR)$(APPS_DIR)/hde-files.desktop $(DESTDIR)$(APPS_DIR)/hde-mimeapps.list

clean:
	rm -f hde-files

.PHONY: install uninstall clean
