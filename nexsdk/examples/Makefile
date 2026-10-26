# nexsdk/examples/Makefile — the NexSDK examples, built against an installed SDK.
#
#   make                      build all of them (pkg-config --cflags --libs nexsdk)
#   make FROM_TREE=1          build against the checkout, without installing anything
#   make clean
CC ?= cc
PKG_CONFIG ?= pkg-config
NEXSDK_CFLAGS := $(shell $(PKG_CONFIG) --cflags nexsdk 2>/dev/null)
NEXSDK_LIBS := $(shell $(PKG_CONFIG) --libs nexsdk 2>/dev/null)
ifeq ($(FROM_TREE),1)
NEXSDK_CFLAGS := -I../include
NEXSDK_LIBS := ../build/libnexsdk.a
endif

CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c11
GTK_CFLAGS := $(shell $(PKG_CONFIG) --cflags gtk+-3.0 2>/dev/null)
GTK_LIBS := $(shell $(PKG_CONFIG) --libs gtk+-3.0 2>/dev/null)

PLAIN := system-info hello-notify settings-theme
ALL := $(PLAIN) gtk-hello

all: $(ALL)

system-info hello-notify settings-theme: %: %.c
	$(CC) $(CFLAGS) $(NEXSDK_CFLAGS) -o $@ $< $(NEXSDK_LIBS)

gtk-hello: gtk-hello.c
	$(CC) $(CFLAGS) $(NEXSDK_CFLAGS) $(GTK_CFLAGS) -o $@ $< $(NEXSDK_LIBS) $(GTK_LIBS)

clean:
	rm -f $(ALL)

.PHONY: all clean
