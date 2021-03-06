.DEFAULT_GOAL := all
CC ?= $(CROSS_COMPILE)gcc
GET_ARCH = ../../external/common/get_arch.sh
include ../../external/common/rules.mk

PREFIX ?= /usr/local/
LIBDIR = $(PREFIX)/lib
INCDIR = $(PREFIX)/include/libflash

VERSION = $(shell ../../make_version.sh)

CFLAGS += -m64 -Werror -Wall -g2 -ggdb -I. -fPIC

.PHONY: links
links: libflash ccan common

libflash:
	ln -sf ../../libflash .

common:
	ln -sf ../common .

ccan:
	ln -sf ../../ccan .

LIBFLASH_OBJS = libflash-file.o libflash-libflash.o libflash-libffs.o libflash-ecc.o libflash-blocklevel.o
ARCHFLASH_OBJS = common-arch_flash.o
OBJS = $(LIBFLASH_OBJS) $(ARCHFLASH_OBJS)

LIBFLASH_H = libflash/file.h libflash/libflash.h libflash/libffs.h libflash/ffs.h libflash/ecc.h libflash/blocklevel.h libflash/errors.h
ARCHFLASH_H = common/arch_flash.h

$(LIBFLASH_OBJS) : libflash-%.o : libflash/%.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) common-*.o *.so*

distclean: clean
	rm -f ccan libflash common

all: links arch_links $(OBJS)
	$(CC) -shared -Wl,-soname,libflash.so -o libflash.so.$(VERSION) $(OBJS)

install-lib: all
	install -D -m 0755 libflash.so.$(VERSION) $(LIBDIR)/libflash.so.$(VERSION)
	ln -sf libflash.so.$(VERSION) $(LIBDIR)/libflash.so

install-dev: links arch_links
	mkdir -p $(INCDIR)
	install -m 0644 $(LIBFLASH_H) $(ARCHFLASH_H) $(INCDIR)

install: install-lib install-dev

uninstall:
	rm -f $(LIBDIR)/libflash*
	rm -rf $(INCDIR)
