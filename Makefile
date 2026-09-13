# openport-j2534 — build, test and install.
# SPDX-License-Identifier: GPL-3.0-or-later

VERSION      := 0.1.0
LIBNAME      := libj2534
UNAME_S      := $(shell uname -s)
UNAME_M      := $(shell uname -m)

PKG_CONFIG   ?= pkg-config
CC           ?= cc

PREFIX       ?= $(if $(filter arm64,$(UNAME_M)),/opt/homebrew,/usr/local)
LIBDIR       ?= $(PREFIX)/lib
BINDIR       ?= $(PREFIX)/bin
INCLUDEDIR   ?= $(PREFIX)/include
PCDIR        ?= $(LIBDIR)/pkgconfig

LIBUSB_CFLAGS := $(shell $(PKG_CONFIG) --cflags libusb-1.0)
LIBUSB_LIBS   := $(shell $(PKG_CONFIG) --libs libusb-1.0)
ifeq ($(strip $(LIBUSB_LIBS)),)
$(error libusb-1.0 not found by $(PKG_CONFIG). Install it: brew install libusb)
endif

ifeq ($(UNAME_S),Darwin)
  SHLIB_EXT  := dylib
  SHLIB_FLAG := -dynamiclib -install_name $(LIBDIR)/$(LIBNAME).dylib
else
  SHLIB_EXT  := so
  SHLIB_FLAG := -shared -Wl,-soname,$(LIBNAME).so
endif

# ARCHS lets CI build a fat binary: make ARCHS="arm64 x86_64"
ARCHS      ?=
ARCH_FLAGS := $(addprefix -arch ,$(ARCHS))

WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion \
            -Wcast-qual -Wstrict-prototypes -Wmissing-prototypes \
            -Wpointer-arith -Wwrite-strings -Wno-unused-parameter
CFLAGS   ?= -O2 -g
CPPFLAGS += -Iinclude -Isrc -std=c11 -DOPENPORT_VERSION='"$(VERSION)"' \
            -D_POSIX_C_SOURCE=200809L $(LIBUSB_CFLAGS)
LDLIBS   += $(LIBUSB_LIBS) -lpthread

SHLIB    := $(LIBNAME).$(SHLIB_EXT)
SRCS     := src/op_proto.c src/op_log.c src/op_error.c src/op_usb.c \
            src/op_serial.c src/op_device.c src/op_j2534.c
OBJS     := $(SRCS:.c=.o)

TEST_SRCS := tests/unit/test_main.c tests/unit/test_proto.c \
             tests/unit/test_frames.c tests/unit/test_j2534.c \
             tests/unit/test_golden.c \
             tests/mock/mock_transport.c
TEST_BIN  := tests/run_tests

.PHONY: all clean test check install uninstall probe tools format smoke kline iso15765
all: $(SHLIB) probe

%.o: %.c
	$(CC) $(ARCH_FLAGS) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -fPIC -c $< -o $@

$(SHLIB): $(OBJS)
	$(CC) $(ARCH_FLAGS) $(SHLIB_FLAG) $(OBJS) $(LDLIBS) -o $@

probe: tools/op_probe
tools/op_probe: tools/probe/op_probe.c
	$(CC) $(ARCH_FLAGS) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $< $(LDLIBS) -o $@

# The unit tests link the library sources directly and inject a mock
# transport, so `make test` passes with no cable attached.
$(TEST_BIN): $(TEST_SRCS) $(SRCS)
	$(CC) $(ARCH_FLAGS) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Itests -Itests/mock -Itests/unit \
	      $(TEST_SRCS) $(SRCS) $(LDLIBS) -o $@

test check: $(TEST_BIN)
	./$(TEST_BIN)

install: $(SHLIB) probe
	install -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(INCLUDEDIR)/j2534 $(DESTDIR)$(PCDIR) $(DESTDIR)$(BINDIR)
	install -m 644 include/j2534/j2534.h $(DESTDIR)$(INCLUDEDIR)/j2534/
	install -m 755 $(SHLIB) $(DESTDIR)$(LIBDIR)/
	install -m 755 tools/op_probe $(DESTDIR)$(BINDIR)/
	sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@LIBDIR@|$(LIBDIR)|g' \
	    -e 's|@INCLUDEDIR@|$(INCLUDEDIR)|g' -e 's|@VERSION@|$(VERSION)|g' \
	    openport-j2534.pc.in > $(DESTDIR)$(PCDIR)/openport-j2534.pc

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/$(SHLIB) $(DESTDIR)$(BINDIR)/op_probe \
	      $(DESTDIR)$(INCLUDEDIR)/j2534/j2534.h \
	      $(DESTDIR)$(PCDIR)/openport-j2534.pc

clean:
	rm -f $(OBJS) $(SHLIB) $(TEST_BIN) tools/op_probe examples/op_smoke examples/op_kline examples/op_iso15765
	rm -rf *.dSYM tests/*.dSYM tools/*.dSYM

smoke: examples/op_smoke
examples/op_smoke: examples/op_smoke.c $(SHLIB)
	$(CC) $(ARCH_FLAGS) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $< \
	      -L. -lj2534 $(LDLIBS) -o $@

kline: examples/op_kline
examples/op_kline: examples/op_kline.c $(SHLIB)
	$(CC) $(ARCH_FLAGS) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $< \
	      -L. -lj2534 $(LDLIBS) -o $@

iso15765: examples/op_iso15765
examples/op_iso15765: examples/op_iso15765.c $(SHLIB)
	$(CC) $(ARCH_FLAGS) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $< \
	      -L. -lj2534 $(LDLIBS) -o $@

# ---- differential harness -------------------------------------------------
# Path to the reference libj2534.dylib to compare against: make differential OLD_DRIVER=...
OLD_DRIVER ?=

diff-tools: tools/usbtap.dylib tests/differential/diff_runner
# The tap is injected into the runner, so both must be the same slice.
# macOS defaults some executables to arm64e; pin both to arm64.
DIFF_ARCH ?= -arch $(UNAME_M)
tools/usbtap.dylib: tools/usbtap/usbtap.c
	$(CC) $(DIFF_ARCH) -dynamiclib $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $< \
	      $(LDLIBS) -o $@
tests/differential/diff_runner: tests/differential/diff_runner.c
	$(CC) $(DIFF_ARCH) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $< -o $@

differential: diff-tools $(SHLIB)
	OLD_DRIVER="$(OLD_DRIVER)" NEW_DRIVER="$(CURDIR)/$(SHLIB)" \
	  tests/differential/run_diff.sh $(DIFF_ARGS)

# ---- A/B against the vendor's Windows DLL (tools/ab-official) --------------
# The DLL runs in a Docker container (box64 + Wine); no Windows machine needed.
#   make ab-official                       # both drivers against the simulator
#   make ab-official-cable CABLE=/dev/cu.usbmodemXXXX
#   make ab-official AB_ARGS="-- open bench --cycles 50"
AB_ARGS ?=
ab-official-image:
	docker build -t openport-ab-official tools/ab-official
ab-official: $(SHLIB)
	tools/ab-official/ab.sh $(AB_ARGS)
ab-official-cable: $(SHLIB)
	tools/ab-official/ab.sh --cable "$(CABLE)" $(AB_ARGS)

# ---- test layers ----------------------------------------------------------
SAN_SRCS := $(TEST_SRCS) $(SRCS)
SAN_FLAGS := $(CPPFLAGS) -g -O1 -Itests -Itests/mock -Itests/unit -fno-omit-frame-pointer

.PHONY: sanitize sanitize-thread fuzz fuzz-deep sim check-all ab-official ab-official-image ab-official-cable
sanitize:
	$(CC) $(DIFF_ARCH) $(SAN_FLAGS) -fsanitize=address,undefined $(SAN_SRCS) $(LDLIBS) -o /tmp/op_t_asan
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 /tmp/op_t_asan

sanitize-thread:
	$(CC) $(DIFF_ARCH) $(SAN_FLAGS) -fsanitize=thread $(SAN_SRCS) $(LDLIBS) -o /tmp/op_t_tsan
	/tmp/op_t_tsan

FUZZ_SRCS := tests/fuzz/fuzz_parse.c tests/fuzz/fuzz_main.c src/op_proto.c
fuzz:
	$(CC) $(DIFF_ARCH) -Iinclude -Isrc -std=c11 -O1 -g -fsanitize=address,undefined \
	      -fno-omit-frame-pointer $(FUZZ_SRCS) -o /tmp/op_fuzz
	ASAN_OPTIONS=detect_leaks=0 /tmp/op_fuzz tests/fuzz/corpus 300000

fuzz-deep:
	$(CC) $(DIFF_ARCH) -Iinclude -Isrc -std=c11 -O1 -g -fsanitize=address,undefined \
	      -fno-omit-frame-pointer $(FUZZ_SRCS) -o /tmp/op_fuzz
	@for s in 1 7 31337 424242; do \
	  ASAN_OPTIONS=detect_leaks=0 /tmp/op_fuzz tests/fuzz/corpus 1500000 $$s || exit 1; \
	done

sim: $(SHLIB)
	OPENPORT_LIB=$(CURDIR)/$(SHLIB) python3 tests/sim/run_scenarios.py

check-all: test sanitize sanitize-thread fuzz sim
	@echo "all hardware-free layers passed"
