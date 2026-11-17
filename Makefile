CC ?= cc
PKG_CONFIG ?= pkg-config
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags libsystemd)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
LDLIBS += $(shell $(PKG_CONFIG) --libs libsystemd)

.PHONY: all clean install

all: ubuntu-core-suspend-test

ubuntu-core-suspend-test: src/suspend-test.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

install: ubuntu-core-suspend-test
	install -D -m 0755 ubuntu-core-suspend-test \
		$(DESTDIR)/usr/bin/ubuntu-core-suspend-test

clean:
	rm -f ubuntu-core-suspend-test
