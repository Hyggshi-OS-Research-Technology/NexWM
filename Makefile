# NexWM Makefile
# Nex Window Manager

CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Wpedantic -std=c11 -O2 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDFLAGS ?= -lxcb -lxcb-util -lxcb-randr -lxcb-ewmh -lxcb-icccm -lxcb-keysyms

DEBUG_CFLAGS = -Wall -Wextra -Wpedantic -std=c11 -g -O0 -DDEBUG \
               -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
               -fsanitize=address,undefined -fno-omit-frame-pointer
DEBUG_LDFLAGS = $(LDFLAGS) -fsanitize=address,undefined

SRCDIR  = src
BINDIR  = bin
PREFIX  ?= /usr/local
BINDIR_INSTALL = $(PREFIX)/bin
ETCDIR  = $(PREFIX)/etc/nexwm

SRCS = $(SRCDIR)/main.c \
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

OBJS = $(SRCS:.c=.o)
DEPS = $(SRCS:.c=.d)

TARGET = $(BINDIR)/nexwm

.PHONY: all debug release clean install uninstall test dirs

all: dirs $(TARGET)

dirs:
	@mkdir -p $(BINDIR)

debug:
	$(MAKE) CFLAGS="$(DEBUG_CFLAGS)" LDFLAGS="$(DEBUG_LDFLAGS)" all

release:
	$(MAKE) CFLAGS="$(CFLAGS) -DNDEBUG" all

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

clean:
	rm -f $(SRCDIR)/*.o $(SRCDIR)/*.d $(TARGET)
	rm -rf $(BINDIR)

install: all
	install -Dm755 $(TARGET) $(BINDIR_INSTALL)/nexwm
	install -Dm644 config/nexwm.conf $(ETCDIR)/nexwm.conf
	@echo "NexWM installed to $(PREFIX)"

uninstall:
	rm -f $(BINDIR_INSTALL)/nexwm
	rm -rf $(ETCDIR)

test:
	@echo "Tests not yet implemented (Phase 11)"
	@echo "Run with Xephyr for manual testing:"
	@echo "  Xephyr :1 -ac -br -noreset -screen 1280x720 &"
	@echo "  DISPLAY=:1 ./$(TARGET) --debug"
