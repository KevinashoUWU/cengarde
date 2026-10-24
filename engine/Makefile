# cengarde engine.
#
# CC, CFLAGS, CPPFLAGS and LDFLAGS belong to whoever builds (cross builds,
# OpenWrt) and may be overridden on the command line; the flags the code
# needs live in the CG_* variables. make SANITIZE=1 builds with ASan and
# UBSan, make SANITIZE=thread with ThreadSanitizer (a build of its own: it
# cannot share one with ASan); HARDEN= and HARDEN_LDFLAGS= drop the
# hardening flags.

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
# gcc warns (-Wtsan) about what TSAN does not model, such as
# atomic_thread_fence (rule R1): an error here. <stdatomic.h> spells it as a
# macro, and gcc drops a warning located in a system header's macro unless
# the function was inlined into our code; -ftrack-macro-expansion=0 puts the
# location at our call, so a fence is caught out of line and at -O0 too.
# clang has neither flag.
TSAN_WERROR = $(shell $(CC) -Werror -Wtsan -ftrack-macro-expansion=0 -fsanitize=thread -E -x c /dev/null -o /dev/null 2>/dev/null && echo -Werror=tsan -ftrack-macro-expansion=0)
CG_CFLAGS += -fsanitize=thread -fno-omit-frame-pointer $(TSAN_WERROR)
CG_LDFLAGS += -fsanitize=thread
else
HARDEN ?= -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE
HARDEN_LDFLAGS ?= -pie -Wl,-z,relro,-z,now
CG_CFLAGS += $(HARDEN)
CG_LDFLAGS += $(HARDEN_LDFLAGS)
endif

# Leopard-RS (third_party/leopard, BSD-3, story 021): redund_spare = rs. C++
# compiled without exceptions, RTTI or the C++ library (its objects need
# only libc, linked by $(CC)); LEOPARD=0 builds without it (no C++ compiler
# needed; rs refused by the configuration). CXX follows a cross CC
# (aarch64-linux-gnu-gcc: aarch64-linux-gnu-g++) unless given. On x86 its
# files are built with SSSE3 (it checks the CPU at run time for its fast
# paths, but the compiler may use SSSE3 anywhere in them).
LEOPARD ?= 1
ifeq ($(origin CXX),default)
CXX = $(patsubst %clang,%clang++,$(patsubst %gcc,%g++,$(patsubst %cc,%c++,$(CC))))
endif
CXXFLAGS ?= -O2 -g
LEO_DIR = third_party/leopard
LEO_SRC = $(LEO_DIR)/leopard.cpp $(LEO_DIR)/LeopardCommon.cpp $(LEO_DIR)/LeopardFF8.cpp
LEO_ARCH := $(shell $(CC) -dumpmachine 2>/dev/null)
LEO_SIMD = $(if $(filter x86_64% i386% i486% i586% i686%,$(LEO_ARCH)),-mssse3)
CG_CXXFLAGS = -std=c++11 -fno-exceptions -fno-rtti -fno-threadsafe-statics $(LEO_SIMD) $(LEO_PORTABLE)
ifeq ($(SANITIZE),1)
CG_CXXFLAGS += -fsanitize=address,undefined -fno-sanitize=vptr,object-size -fno-omit-frame-pointer -fno-sanitize-recover=all
else ifeq ($(SANITIZE),thread)
CG_CXXFLAGS += -fsanitize=thread -fno-omit-frame-pointer
else
CG_CXXFLAGS += $(HARDEN)
endif
# (UBSan's object-size check: off for Leopard, whose FFT indexes its tables
# through a pointer one before them, FFTSkew - 1; ASan still checks every
# access against the real objects)
# one object directory per kind of build: sanitizers and compilers do not mix
LEO_OBJDIR = obj/leopard-$(if $(SANITIZE),san$(SANITIZE),plain)$(if $(LEO_PORTABLE),-portable)
ifeq ($(LEOPARD),0)
LEO_OBJ =
CG_CFLAGS += -DCG_NO_LEOPARD
else
LEO_OBJ = $(patsubst $(LEO_DIR)/%.cpp,$(LEO_OBJDIR)/%.o,$(LEO_SRC))
CG_CFLAGS += -I$(LEO_DIR)
endif

CORE = src/siphash.c src/proto.c src/util.c src/log.c src/config.c src/blake2s.c src/pair.c src/rs.c
NET = src/netlink.c src/sock.c src/tun.c src/status.c src/os.c src/ctl.c src/names.c src/client.c src/server.c
# The router's link pumps: threads the unit tests run too (test_threads).
THR = src/pump.c
# Every tests/test_*.c; tests/suites.h lists the suites test_main.c runs.
TESTS = $(wildcard tests/test_*.c)

all: cengarde

cengarde: src/main.c $(CORE) $(THR) $(NET) $(wildcard src/*.h) $(LEO_OBJ)
	$(CC) $(CG_CFLAGS) $(CPPFLAGS) $(CFLAGS) -o $@ src/main.c $(CORE) $(THR) $(NET) $(LEO_OBJ) $(LDFLAGS) $(CG_LDFLAGS)

cengarde-test: $(TESTS) $(CORE) $(THR) $(wildcard src/*.h tests/*.h) $(LEO_OBJ)
	$(CC) $(CG_CFLAGS) $(CPPFLAGS) $(CFLAGS) -Isrc -o $@ $(TESTS) $(CORE) $(THR) $(LEO_OBJ) $(LDFLAGS) $(CG_LDFLAGS)

$(LEO_OBJDIR)/%.o: $(LEO_DIR)/%.cpp $(wildcard $(LEO_DIR)/*.h) Makefile
	@mkdir -p $(LEO_OBJDIR)
	$(CXX) $(CG_CXXFLAGS) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

test: cengarde-test
	./cengarde-test

install: cengarde
	install -D -m 0755 cengarde $(DESTDIR)$(PREFIX)/sbin/cengarde

clean:
	rm -f cengarde cengarde-test
	rm -rf obj

.PHONY: all test install clean
