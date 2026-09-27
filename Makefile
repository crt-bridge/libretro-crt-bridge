# SPDX-License-Identifier: GPL-3.0-or-later
#
# crt-bridge client -- builds the RetroArch core (crt_bridge_libretro), its
# .info file, and the unit tests, on Windows, Linux and macOS (universal
# arm64 + x86_64), from the files in this repository alone.
#
# Windows: from an MSYS2 MINGW64 shell, `make`. From Git Bash or PowerShell,
#          `mingw32-make` with C:\msys64\mingw64\bin first on PATH -- NEVER
#          the make under C:\msys64\usr\bin (Git Bash's TMP gets rewritten to
#          a POSIX form there and gcc falls over on C:\WINDOWS). The CI
#          workflow (release.yml) runs a real MSYS2 MINGW64 shell, so it
#          uses plain `make` -- this is not a contradiction of the rule above,
#          which is about invoking make from OUTSIDE that shell.
# Linux:   make
# macOS:   make            (builds both the arm64 and x86_64 slices)
#
# GNU Make 3.81 minimum: this file uses no feature newer than that version.

UNAME := $(shell uname -s)

BUILD  := build
ARCHS  :=

# CFLAGS is the caller's own knob (`make CFLAGS=...`, or inherited from
# a packaging environment such as makepkg/Gentoo, both common for a public
# repository) -- `?=` sets nothing when it already has a value from there,
# silently dropping -Isrc/-Iinclude/-Ideps and -std=c11 and breaking the
# build on "gmclient.h: No such file". ALL_CFLAGS carries what this build
# REQUIRES, always, on top of whatever CFLAGS the caller supplied; every
# recipe below compiles with $(ALL_CFLAGS), never bare $(CFLAGS).
CFLAGS     ?= -O2 -Wall -Wextra -Wshadow
ALL_CFLAGS := -std=c11 -Isrc -Iinclude -Ideps $(CFLAGS)

# The '#' below is backslash-escaped: without it, make treats an unescaped
# '#' as starting a comment even inside a function call argument, on the
# real GNU Make 3.81 this file targets (measured: it silently truncates this
# whole $(shell ...) call, which then reads as an unterminated function).
VERSION := $(shell sed -n 's/^\#define CRT_BRIDGE_CLIENT_VERSION "\([0-9.]*\)"$$/\1/p' src/core/crt_bridge_version.h)
ifeq ($(VERSION),)
$(error cannot read CRT_BRIDGE_CLIENT_VERSION from src/core/crt_bridge_version.h)
endif

ifneq (,$(findstring MINGW,$(UNAME))$(findstring MSYS,$(UNAME)))
PLATFORM          := windows
CC                := gcc
CORE_EXT          := dll
CORE_SHARED_FLAGS := -shared
CORE_LDLIBS       := -lws2_32 -lwinmm -static-libgcc
TEST_LDLIBS       := -lws2_32 -lwinmm
FROZEN_LDLIBS     := -lpthread
else ifeq ($(UNAME),Darwin)
PLATFORM          := macos
# `CC ?= clang` is a no-op -- GNU Make's own built-in default for CC
# is already `cc` with origin "default", and `?=` treats that as "already
# has a value". $(origin CC) is the actual test for "nothing set it yet"; a
# real override (command line, environment, an earlier Makefile assignment)
# still wins either way.
ifeq ($(origin CC),default)
CC := clang
endif
# Without an explicit minimum, clang stamps LC_BUILD_VERSION with the
# CI RUNNER'S OWN SDK version (macOS 14 or 15 today) -- the x86_64 slice
# targets Intel Macs that often run older systems than the runner that built
# it. arm64 does not exist before 11.0, so 11.0 costs nothing on that slice.
ARCHS             := -arch arm64 -arch x86_64 -mmacosx-version-min=11.0
CORE_EXT          := dylib
CORE_SHARED_FLAGS := -dynamiclib
CORE_LDLIBS       := -lpthread
TEST_LDLIBS       := -lpthread
FROZEN_LDLIBS     := -lpthread
else ifeq ($(UNAME),Linux)
PLATFORM          := linux
ifeq ($(origin CC),default)
CC := gcc
endif
# glibc under strict -std=c11 enables __STRICT_ANSI__, which disables
# _DEFAULT_SOURCE: <time.h> then declares neither clock_gettime nor
# CLOCK_MONOTONIC, and <sys/select.h> can hide select() (measured, this
# project's own private client/Makefile carries the identical note). This is
# a build REQUIREMENT (the same point made about CFLAGS above) -- ALL_CFLAGS, never the caller's
# overridable CFLAGS.
ALL_CFLAGS        += -D_POSIX_C_SOURCE=200809L
CORE_EXT          := so
CORE_SHARED_FLAGS := -fPIC -shared
CORE_LDLIBS       := -lpthread
TEST_LDLIBS       := -lpthread
FROZEN_LDLIBS     := -lpthread -ldl
else
PLATFORM          := unknown
endif

ifeq ($(PLATFORM),unknown)
$(error unrecognized platform (uname -s = $(UNAME)))
endif

.DEFAULT_GOAL := all
.PHONY: all test test-frozen-run print-version clean

$(BUILD):
	mkdir -p $(BUILD)

CORE_LIB  := $(BUILD)/crt_bridge_libretro.$(CORE_EXT)
CORE_INFO := $(BUILD)/crt_bridge_libretro.info

all: $(CORE_LIB) $(CORE_INFO)

print-version:
	@echo $(VERSION)

# --- core -------------------------------------------------------------------
# deps/lz4.c is passed as a source file to this single compile-and-link
# command, never precompiled into its own object file: a universal (arm64 +
# x86_64) macOS build needs every object it links to carry both slices, and a
# single .o built once and reused would only ever hold one.
CORE_SRCS := src/core/crt_bridge_libretro.c src/gmclient.c src/gmclient_input.c deps/lz4.c
CORE_DEPS := src/core/crt_bridge_libretro.c src/core/crt_bridge_version.h \
             src/core/loss_hint.h src/gmclient.c src/gmclient.h \
             src/gmclient_input.c src/gmclient_input.h src/gmc_mtx.h \
             deps/lz4.c deps/lz4.h include/libretro.h

$(CORE_LIB): $(CORE_DEPS) | $(BUILD)
	$(CC) $(ALL_CFLAGS) $(ARCHS) $(CORE_SHARED_FLAGS) -o $@ $(CORE_SRCS) $(CORE_LDLIBS)

# Depends on the version header too -- a rule that only named the
# .info.in template rebuilt the DLL (which depends on the header, via
# CORE_DEPS) after a version bump, but kept the STALE .info from before it on
# an incremental `make`, which check-release.py then fails on
# display_version. A from-scratch clone (the CI's own path) was never
# affected -- both files are always missing there.
$(CORE_INFO): crt_bridge_libretro.info.in src/core/crt_bridge_version.h | $(BUILD)
	sed 's/@VERSION@/$(VERSION)/' crt_bridge_libretro.info.in > $@

# --- tests -------------------------------------------------------------------
# Each test binary includes its own subject .c file directly (GMCLIENT_TEST),
# so it is never linked against a separately compiled object -- no duplicate
# symbols. test_gmclient_audio and test_gmclient_padding pull gmclient.c,
# which needs lz4.h: deps/lz4.c is added to their link line for the same
# reason it is added to the core's, above.

$(BUILD)/test_gmclient_input: test/test_gmclient_input.c src/gmclient_input.c src/gmclient_input.h src/gmc_mtx.h | $(BUILD)
	$(CC) $(ALL_CFLAGS) $(ARCHS) -o $@ test/test_gmclient_input.c $(TEST_LDLIBS)

$(BUILD)/test_gmclient_audio: test/test_gmclient_audio.c src/gmclient.c src/gmclient.h deps/lz4.c deps/lz4.h | $(BUILD)
	$(CC) $(ALL_CFLAGS) $(ARCHS) -o $@ test/test_gmclient_audio.c deps/lz4.c $(TEST_LDLIBS)

$(BUILD)/test_gmclient_padding: test/test_gmclient_padding.c src/gmclient.c src/gmclient.h deps/lz4.c deps/lz4.h | $(BUILD)
	$(CC) $(ALL_CFLAGS) $(ARCHS) -o $@ test/test_gmclient_padding.c deps/lz4.c $(TEST_LDLIBS)

$(BUILD)/test_loss_hint: test/test_loss_hint.c src/core/loss_hint.h | $(BUILD)
	$(CC) $(ALL_CFLAGS) $(ARCHS) -o $@ test/test_loss_hint.c $(TEST_LDLIBS)

test: $(BUILD)/test_gmclient_input $(BUILD)/test_gmclient_audio $(BUILD)/test_gmclient_padding $(BUILD)/test_loss_hint
	$(BUILD)/test_gmclient_input
	$(BUILD)/test_gmclient_audio
	$(BUILD)/test_gmclient_padding
	$(BUILD)/test_loss_hint

# --- frozen-run harness -------------------------------------------------------
# Loads the built core dynamically (LoadLibraryA/dlopen) and checks it
# survives a video_cb that blocks forever, without ever starting RetroArch.
# GMC_PORT (default 42100, read by the harness itself) lets several builds
# run this target at once without a port clash.
FROZEN_REPORT := $(BUILD)/frozen-run.json

$(BUILD)/test_frozen_run: test/test_frozen_run.c include/libretro.h | $(BUILD)
	$(CC) $(ALL_CFLAGS) $(ARCHS) -o $@ test/test_frozen_run.c $(FROZEN_LDLIBS)

test-frozen-run: $(CORE_LIB) $(BUILD)/test_frozen_run
	$(BUILD)/test_frozen_run freeze $(CORE_LIB) $(FROZEN_REPORT)
	$(BUILD)/test_frozen_run check $(FROZEN_REPORT) 1
	$(BUILD)/test_frozen_run clean $(CORE_LIB) $(FROZEN_REPORT)

clean:
	rm -rf $(BUILD)
