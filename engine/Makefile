# cengarde engine.
#
# CC, CFLAGS, CPPFLAGS and LDFLAGS belong to whoever builds (cross builds,
# OpenWrt) and may be overridden on the command line; the flags the code
# needs live in the CG_* variables. make SANITIZE=1 builds with ASan and
# UBSan, make SANITIZE=thread with TSAN; HARDEN= and HARDEN_LDFLAGS= drop
# the hardening flags.

CC ?= cc
CFLAGS ?= -O2 -g
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo unknown)
PREFIX ?= /usr

CG_CFLAGS = -std=c11 -D_GNU_SOURCE -DCG_VERSION=\"$(VERSION)\" -pthread \
            -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes -Wmissing-prototypes \
            -Wformat=2 -Wno-unused-parameter -Wvla
CG_LDFLAGS = -pthread
ifeq ($(SANITIZE),1)
CG_CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
CG_LDFLAGS += -fsanitize=address,undefined
else ifeq ($(SANITIZE),thread)
CG_CFLAGS += -fsanitize=thread -fno-omit-frame-pointer
CG_LDFLAGS += -fsanitize=thread
else
HARDEN ?= -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE
HARDEN_LDFLAGS ?= -pie -Wl,-z,relro,-z,now
CG_CFLAGS += $(HARDEN)
CG_LDFLAGS += $(HARDEN_LDFLAGS)
endif

CORE = src/siphash.c src/proto.c src/util.c src/log.c src/config.c src/blake2s.c src/pair.c
NET = src/netlink.c src/sock.c src/status.c src/os.c src/ctl.c src/client.c src/server.c
# Every tests/test_*.c; tests/suites.h lists the suites test_main.c runs.
TESTS = $(wildcard tests/test_*.c)

all: cengarde

cengarde: src/main.c $(CORE) $(NET) $(wildcard src/*.h)
	$(CC) $(CG_CFLAGS) $(CPPFLAGS) $(CFLAGS) -o $@ src/main.c $(CORE) $(NET) $(LDFLAGS) $(CG_LDFLAGS)

cengarde-test: $(TESTS) $(CORE) $(wildcard src/*.h tests/*.h)
	$(CC) $(CG_CFLAGS) $(CPPFLAGS) $(CFLAGS) -Isrc -o $@ $(TESTS) $(CORE) $(LDFLAGS) $(CG_LDFLAGS)

test: cengarde-test
	./cengarde-test

install: cengarde
	install -D -m 0755 cengarde $(DESTDIR)$(PREFIX)/sbin/cengarde

clean:
	rm -f cengarde cengarde-test

.PHONY: all test install clean
