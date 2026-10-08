# HDE Cmd (hde-cmd): its source, tests and build live in this folder. The top-level Makefile also builds/installs it.
#
#   make              build ./hde-cmd (GTK3; the built-in VT engine has no VTE dependency)
#   make check        run the plain-C VT parser tests without a display
#   sudo make install  install /usr/local/bin/hde-cmd and its desktop entry
#
# VTE is an optional, explicitly selected backend. It is detected at build time but is not needed to build the default
# built-in VT engine. `hde-cmd --vte` uses it when present.
CC ?= cc
PREFIX ?= /usr/local
APPS_DIR ?= /usr/share/applications
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c11
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
GTK_LIBS := $(shell pkg-config --libs gtk+-3.0 2>/dev/null)
VTE_PC := $(shell pkg-config --exists vte-2.91 2>/dev/null && echo vte-2.91)
VTE_CFLAGS := $(shell [ -n "$(VTE_PC)" ] && echo "-DHDE_CMD_HAVE_VTE `pkg-config --cflags $(VTE_PC)`")
VTE_LIBS := $(shell [ -n "$(VTE_PC)" ] && pkg-config --libs $(VTE_PC))
SRC = $(wildcard src/*.c)
HEADERS = $(wildcard src/*.h)

hde-cmd: $(SRC) $(HEADERS)
	$(CC) $(CFLAGS) -Isrc $(GTK_CFLAGS) $(VTE_CFLAGS) -o $@ $(filter %.c,$^) $(GTK_LIBS) $(VTE_LIBS) -lutil -lm

BUILD_DIR ?= ../build

$(BUILD_DIR)/cmd-vt-test: ../tests/cmd-vt-test.c src/vt.c src/vt.h
	mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Wpedantic -std=c11 -Isrc -o $@ ../tests/cmd-vt-test.c src/vt.c

check: $(BUILD_DIR)/cmd-vt-test
	$(BUILD_DIR)/cmd-vt-test

install: hde-cmd
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(APPS_DIR)
	install -m755 hde-cmd $(DESTDIR)$(PREFIX)/bin/hde-cmd
	sed 's|@PREFIX@|$(PREFIX)|g' hde-cmd.desktop > $(DESTDIR)$(APPS_DIR)/hde-cmd.desktop
	chmod 644 $(DESTDIR)$(APPS_DIR)/hde-cmd.desktop
	-update-desktop-database $(DESTDIR)$(APPS_DIR) 2>/dev/null

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/hde-cmd $(DESTDIR)$(APPS_DIR)/hde-cmd.desktop

clean:
	rm -f hde-cmd

.PHONY: check install uninstall clean
