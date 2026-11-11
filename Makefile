# openport-j2534 — build, test and install.
# SPDX-License-Identifier: GPL-3.0-or-later

VERSION      := 0.4.2
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

# A Windows build is any build whose compiler targets Windows: MinGW-w64,
# cross from macOS or Linux or native under MSYS2.
#   make CC=x86_64-w64-mingw32-gcc      # 64-bit
#   make CC=i686-w64-mingw32-gcc        # 32-bit; most J2534 applications
# Everything it makes goes under build/win64 or build/win32, so both can be
# built from one checkout, beside a native build.
ifneq ($(findstring mingw,$(shell $(CC) -dumpmachine 2>/dev/null)),)
  WINDOWS  := 1
  WIN_BITS := $(if $(findstring x86_64,$(shell $(CC) -dumpmachine)),64,32)
endif

ifndef WINDOWS
LIBUSB_CFLAGS := $(shell $(PKG_CONFIG) --cflags libusb-1.0)
LIBUSB_LIBS   := $(shell $(PKG_CONFIG) --libs libusb-1.0)
ifeq ($(strip $(LIBUSB_LIBS)),)
$(error libusb-1.0 not found by $(PKG_CONFIG). Install it: brew install libusb)
endif
endif

# A program linked against this checkout finds the library through its rpath,
# so it runs the checkout's build, not an installed one; install gives the
# installed copy its own path.
ifdef WINDOWS
  # B is where objects go; the programs sit beside the DLL, where Windows
  # looks for it first. Both DLLs can be copied into one folder, hence the
  # bits in the name.
  B          := build/win$(WIN_BITS)/
  X          := .exe
  SHLIB      := $(B)openport-j2534-$(WIN_BITS).dll
  IMPLIB     := $(B)libopenport-j2534-$(WIN_BITS).dll.a
  TOOLS_DIR  := $(B)
  EX_DIR     := $(B)
  TESTS_DIR  := $(B)
  DLLTOOL    ?= $(patsubst %gcc,%dlltool,$(CC))
  # 32-bit exports are __stdcall; see src/j2534-x86.def.
  ifeq ($(WIN_BITS),32)
    DEF        := src/j2534-x86.def
    SHLIB_FLAG := -shared -Wl,--kill-at
    DLLTOOL_FLAGS := --kill-at
  else
    DEF        := src/j2534.def
    SHLIB_FLAG := -shared
    DLLTOOL_FLAGS :=
  endif
  # -static: the DLL carries winpthreads and libgcc, so it needs no other
  # file beside it.
  PLATFORM_LIBS := -lsetupapi -lwinmm -static
  # MinGW's own C99 printf, not Microsoft's: the commands sent to the cable
  # are formatted with %zu.
  CPPFLAGS += -D__USE_MINGW_ANSI_STDIO=1
  # Runs a Windows program: Wine when the build machine is not Windows.
  RUN ?= $(if $(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)),,wine)
else ifeq ($(UNAME_S),Darwin)
  SHLIB_EXT  := dylib
  SHLIB_FLAG := -dynamiclib -install_name @rpath/$(LIBNAME).dylib
  INSTALL_ID  = install_name_tool -id $(LIBDIR)/$(SHLIB) $(DESTDIR)$(LIBDIR)/$(SHLIB)
  PLATFORM_LIBS := -framework IOKit -framework CoreFoundation
else
  SHLIB_EXT  := so
  SHLIB_FLAG := -shared -Wl,-soname,$(LIBNAME).so
  INSTALL_ID  = true
endif
ifdef WINDOWS
IN_TREE := $(IMPLIB)
else
B         :=
X         :=
SHLIB     := $(LIBNAME).$(SHLIB_EXT)
TOOLS_DIR := tools/
EX_DIR    := examples/
TESTS_DIR := tests/
PIC       := -fPIC
IN_TREE   := -L. -lj2534 -Wl,-rpath,$(CURDIR)
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
LDLIBS   += $(LIBUSB_LIBS) -lpthread $(PLATFORM_LIBS)

# The byte pipe to the cable is the one part that differs by platform.
ifdef WINDOWS
TRANSPORT_SRCS := src/op_win.c
DLL_SRCS       := src/op_winreg.c
PROBE_SRC      := tools/probe/op_probe_win.c
else
TRANSPORT_SRCS := src/op_usb.c src/op_serial.c
DLL_SRCS       :=
PROBE_SRC      := tools/probe/op_probe.c
endif
SRCS     := src/op_proto.c src/op_log.c src/op_error.c $(TRANSPORT_SRCS) \
            src/op_device.c src/op_j2534.c
OBJS     := $(patsubst %.c,$(B)%.o,$(SRCS) $(DLL_SRCS))

TEST_SRCS := tests/unit/test_main.c tests/unit/test_proto.c \
             tests/unit/test_frames.c tests/unit/test_j2534.c \
             tests/unit/test_golden.c \
             tests/mock/mock_transport.c
TEST_BIN  := $(TESTS_DIR)run_tests$(X)

.PHONY: all clean test check install uninstall probe tools format smoke kline iso15765
all: $(SHLIB) probe

$(B)%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(ARCH_FLAGS) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -MMD -MP $(PIC) -c $< -o $@

# Rebuild an object when a header it includes changes: a struct edited in
# op_device.h otherwise links old and new layouts into one library. The
# Makefile holds VERSION, which op_j2534.o reports.
-include $(OBJS:.o=.d)
$(OBJS): Makefile

ifdef WINDOWS
# The import library is what a C program links against; the examples do.
$(SHLIB): $(OBJS) $(DEF) Makefile
	$(CC) $(SHLIB_FLAG) $(OBJS) $(DEF) $(LDLIBS) -o $@
	$(DLLTOOL) $(DLLTOOL_FLAGS) -d $(DEF) -D $(notdir $(SHLIB)) -l $(IMPLIB)
else
$(SHLIB): $(OBJS) Makefile
	$(CC) $(ARCH_FLAGS) $(SHLIB_FLAG) $(OBJS) $(LDLIBS) -o $@
endif

PROBE_BIN := $(TOOLS_DIR)op_probe$(X)
probe: $(PROBE_BIN)
$(PROBE_BIN): $(PROBE_SRC)
	@mkdir -p $(@D)
	$(CC) $(ARCH_FLAGS) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $< $(LDLIBS) -o $@

# The unit tests link the library sources directly and inject a mock
# transport, so `make test` passes with no cable attached.
$(TEST_BIN): $(TEST_SRCS) $(SRCS)
	@mkdir -p $(@D)
	$(CC) $(ARCH_FLAGS) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Itests -Itests/mock -Itests/unit \
	      $(TEST_SRCS) $(SRCS) $(LDLIBS) -o $@

test check: $(TEST_BIN)
	$(RUN) ./$(TEST_BIN)

ifdef WINDOWS
install uninstall:
	@echo "Windows has no install step: keep $(SHLIB) where you want it and run regsvr32 on it (README.md)." >&2; exit 1
else
install: $(SHLIB) probe
	install -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(INCLUDEDIR)/j2534 $(DESTDIR)$(PCDIR) $(DESTDIR)$(BINDIR)
	install -m 644 include/j2534/j2534.h $(DESTDIR)$(INCLUDEDIR)/j2534/
	install -m 755 $(SHLIB) $(DESTDIR)$(LIBDIR)/
	$(INSTALL_ID)
	install -m 755 tools/op_probe $(DESTDIR)$(BINDIR)/
	sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@LIBDIR@|$(LIBDIR)|g' \
	    -e 's|@INCLUDEDIR@|$(INCLUDEDIR)|g' -e 's|@VERSION@|$(VERSION)|g' \
	    openport-j2534.pc.in > $(DESTDIR)$(PCDIR)/openport-j2534.pc

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/$(SHLIB) $(DESTDIR)$(BINDIR)/op_probe \
	      $(DESTDIR)$(INCLUDEDIR)/j2534/j2534.h \
	      $(DESTDIR)$(PCDIR)/openport-j2534.pc
endif

# A native clean leaves the Windows builds alone, and the other way round.
clean:
ifdef WINDOWS
	rm -rf $(B)
else
	rm -f $(OBJS) $(OBJS:.o=.d) $(SHLIB) $(TEST_BIN) tools/op_probe examples/op_smoke examples/op_kline examples/op_iso15765
	rm -rf *.dSYM tests/*.dSYM tools/*.dSYM
endif

smoke: $(EX_DIR)op_smoke$(X)
kline: $(EX_DIR)op_kline$(X)
iso15765: $(EX_DIR)op_iso15765$(X)
$(EX_DIR)op_%$(X): examples/op_%.c $(SHLIB)
	$(CC) $(ARCH_FLAGS) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $< \
	      $(IN_TREE) $(LDLIBS) -o $@

# Everything below is the native test rig: it needs ptys, sanitizers and
# dlopen. The Windows build is tested by `make test` and `make wine-sim`.
ifndef WINDOWS
# ---- differential harness -------------------------------------------------
# Path to the reference libj2534.dylib to compare against: make differential OLD_DRIVER=...
OLD_DRIVER ?=

# The tap is injected into the runner, so both must be the same slice; macOS
# defaults some executables to arm64e. -arch is a Darwin flag, and the
# sanitizer and fuzz targets below carry it too, so elsewhere it stays empty.
ifeq ($(UNAME_S),Darwin)
DIFF_ARCH ?= -arch $(UNAME_M)
diff-tools: tools/usbtap.dylib tests/differential/diff_runner
else
DIFF_ARCH ?=
diff-tools:
	@echo "diff-tools: usbtap is a DYLD_INSERT_LIBRARIES interposer and needs macOS" >&2; exit 1
endif
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
#   make ab-official AB_ARGS="-- sweep"    # every parameter once, wire compared per step
#   make ab-official AB_ARGS="-- edge"     # values J2534 forbids; on a cable run it alone, last
AB_ARGS ?=
ab-official-image:
	docker build --label "ab.context=$$(cat tools/ab-official/Dockerfile tools/ab-official/iface.reg | shasum -a 256 | cut -c1-16)" -t openport-ab-official tools/ab-official
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

# The Windows DLLs under Wine against this build, on the simulator
# (docs/TESTING.md). Needs MinGW-w64 and Wine, so check-all leaves it out.
.PHONY: wine-sim
wine-sim: $(SHLIB)
	tests/windows/run_wine.sh 32
	tests/windows/run_wine.sh 64

check-all: test sanitize sanitize-thread fuzz sim
	@echo "all hardware-free layers passed"
endif
