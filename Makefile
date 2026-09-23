# ═══════════════════════════════════════════════════════════════════════════════
# ytcui — Cross-platform Makefile (Linux, macOS, FreeBSD)
# ═══════════════════════════════════════════════════════════════════════════════

# ─── OS Detection ───────────────────────────────────────────────────────────────
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
    OS_TYPE := macos
else ifeq ($(UNAME_S),FreeBSD)
    OS_TYPE := freebsd
else ifeq ($(UNAME_S),DragonFly)
    OS_TYPE := dragonfly
else ifeq ($(UNAME_S),NetBSD)
    OS_TYPE := netbsd
else ifeq ($(UNAME_S),OpenBSD)
    OS_TYPE := openbsd
else
    OS_TYPE := linux
endif

# ─── Compiler Selection ─────────────────────────────────────────────────────────
# Prefer g++ on Linux, clang++ on macOS/BSD (unless CXX is set)
ifeq ($(origin CXX),default)
    ifeq ($(OS_TYPE),macos)
        CXX := clang++
    else ifeq ($(OS_TYPE),freebsd)
        CXX := clang++
    else
        CXX := g++
    endif
endif

# ─── Base Flags ─────────────────────────────────────────────────────────────────
CXXFLAGS += -std=c++17 -Wall -Wextra -O3 -Iinclude

# Required for wide-char functions (ncursesw)
CXXFLAGS += -D_XOPEN_SOURCE_EXTENDED

# ─── Platform-specific defines ──────────────────────────────────────────────────
ifeq ($(OS_TYPE),macos)
    CXXFLAGS += -DYTUI_MACOS
else ifeq ($(OS_TYPE),freebsd)
    CXXFLAGS += -DYTUI_FREEBSD
else ifeq ($(OS_TYPE),dragonfly)
    CXXFLAGS += -DYTUI_FREEBSD
else
    CXXFLAGS += -DYTUI_LINUX
endif

# ─── ncurses Detection ──────────────────────────────────────────────────────────
# CRITICAL: Must use ncursesw (wide-char ncurses) for proper UTF-8 rendering.

ifeq ($(OS_TYPE),macos)
    NCURSES_PREFIX ?= /opt/local

    ifneq ($(wildcard $(NCURSES_PREFIX)/lib/libncursesw.*),)
        NCURSES_CFLAGS := -I$(NCURSES_PREFIX)/include
        NCURSES_LIBS := -L$(NCURSES_PREFIX)/lib -lncursesw
    else
        # Fallback to system ncurses
        NCURSES_CFLAGS :=
        NCURSES_LIBS := -lncurses
    endif
else ifeq ($(OS_TYPE),freebsd)
    # FreeBSD: ncurses is in base system
    NCURSES_CFLAGS :=
    NCURSES_LIBS := -lncursesw
else
    # Linux: Use pkg-config
    NCURSES_LIBS := $(shell pkg-config --libs ncursesw 2>/dev/null || echo "-lncursesw")
    NCURSES_CFLAGS := $(shell pkg-config --cflags ncursesw 2>/dev/null || echo "")
endif

CXXFLAGS += $(NCURSES_CFLAGS)

# ─── libcurl + libcrypto (native MissAV provider) ───────────────────────────────
# src/missav.cpp talks to MissAV's Recombee backend directly: libcurl for HTTP,
# libcrypto for the HMAC-SHA1 request signing. Both are overridable so a
# packager can point CURL_* at curl-impersonate for browser TLS fingerprints:
#
#   make CURL_CFLAGS="-I<prefix>/include" CURL_LIBS="-L<prefix>/lib -lcurl-impersonate"
#
# Without an override, curl-impersonate is used when found in a usual prefix:
# MissAV's pages and stream CDN reject stock libcurl's TLS fingerprint outright.
# `make CURL_IMPERSONATE=0` forces stock libcurl. Headers come from libcurl
# either way — curl-impersonate is ABI-compatible, and its one extra entry point
# is looked up at run time (see Http::impersonate).
CURL_CFLAGS   ?= $(shell pkg-config --cflags libcurl 2>/dev/null)
ifneq ($(CURL_IMPERSONATE),0)
    CURL_IMPERSONATE_LIB := $(firstword $(wildcard \
        $(HOME)/.local/lib/libcurl-impersonate.so \
        /usr/local/lib/libcurl-impersonate.so \
        /usr/lib/libcurl-impersonate.so \
        /usr/lib/x86_64-linux-gnu/libcurl-impersonate.so \
        /usr/lib/aarch64-linux-gnu/libcurl-impersonate.so \
        /opt/local/lib/libcurl-impersonate.dylib \
        /opt/homebrew/lib/libcurl-impersonate.dylib))
endif
ifneq ($(CURL_IMPERSONATE_LIB),)
    CURL_IMPERSONATE_DIR := $(patsubst %/,%,$(dir $(CURL_IMPERSONATE_LIB)))
    CURL_LIBS ?= -L$(CURL_IMPERSONATE_DIR) -Wl,-rpath,$(CURL_IMPERSONATE_DIR) -lcurl-impersonate
endif
CURL_LIBS     ?= $(shell pkg-config --libs libcurl 2>/dev/null || echo "-lcurl")
CRYPTO_CFLAGS ?= $(shell pkg-config --cflags libcrypto 2>/dev/null)
CRYPTO_LIBS   ?= $(shell pkg-config --libs libcrypto 2>/dev/null || echo "-lcrypto")

CXXFLAGS += $(CURL_CFLAGS) $(CRYPTO_CFLAGS)

# dlsym/dladdr (Http::impersonate). In libc on glibc >= 2.34 and the BSDs.
ifeq ($(OS_TYPE),linux)
    DL_LIBS := -ldl
endif

# ─── nlohmann/json (header-only) ────────────────────────────────────────────────
# A copy is vendored in include/vendor/, which is NOT on the include path by
# default-of-itself — it is added only when we mean to use it. That way
# <nlohmann/json.hpp> resolves to exactly one place, instead of depending on the
# order of -I flags (the vendored copy sat in include/ before, so -Iinclude
# always shadowed any system header).
#
#   make SYSTEM_JSON=1     use the system nlohmann-json (MacPorts: nlohmann-json)
#   make                   use the vendored copy, if present
#
# Deleting include/vendor also works: detection falls back to the system header,
# so a packager can strip the bundled copy without editing this file.
VENDORED_JSON := $(wildcard include/vendor/nlohmann/json.hpp)

ifeq ($(SYSTEM_JSON),1)
    USE_SYSTEM_JSON := 1
else ifeq ($(VENDORED_JSON),)
    USE_SYSTEM_JSON := 1
else
    USE_SYSTEM_JSON := 0
endif

ifeq ($(USE_SYSTEM_JSON),1)
    # pkg-config may legitimately return nothing when the header is already on
    # the default search path (/usr/include, /opt/local/include via the port
    # compiler wrappers); that is fine, the include just resolves normally.
    JSON_CFLAGS ?= $(shell pkg-config --cflags nlohmann_json 2>/dev/null)
    JSON_ORIGIN := system
else
    JSON_CFLAGS := -Iinclude/vendor
    JSON_ORIGIN := vendored (include/vendor)
endif

CXXFLAGS += $(JSON_CFLAGS)

LDFLAGS = $(NCURSES_LIBS) $(CURL_LIBS) $(CRYPTO_LIBS) -lpthread $(DL_LIBS)

# ─── Directories ────────────────────────────────────────────────────────────────
SRC_DIR = src
INC_DIR = include
OBJ_DIR = build
BIN_DIR = .

SOURCES = $(wildcard $(SRC_DIR)/*.cpp)
OBJECTS = $(SOURCES:$(SRC_DIR)/%.cpp=$(OBJ_DIR)/%.o)
TARGET = $(BIN_DIR)/avcui

VERSION := $(shell cat VERSION 2>/dev/null || echo "0.1.0")

# ─── Targets ────────────────────────────────────────────────────────────────────
.PHONY: all clean install uninstall version info

all: info $(TARGET)

info:
	@echo "Building avcui v$(VERSION) for $(OS_TYPE) using $(CXX)"
	@echo "  nlohmann/json: $(JSON_ORIGIN)"
	@echo "  libcurl:       $(if $(findstring curl-impersonate,$(CURL_LIBS)),curl-impersonate ($(CURL_LIBS)),stock ($(CURL_LIBS)) - MissAV pages/streams will 403)"

$(TARGET): $(OBJECTS) | $(BIN_DIR)
	$(CXX) $(OBJECTS) -o $@ $(LDFLAGS)
	@echo "Build complete: $@ (v$(VERSION))"

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

clean:
	rm -rf $(OBJ_DIR) $(TARGET)
	@echo "Clean complete"

version:
	@echo "$(VERSION)"

PREFIX ?= /usr/local

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/
	install -d $(DESTDIR)$(PREFIX)/share/avcui
	install -m 644 VERSION $(DESTDIR)$(PREFIX)/share/avcui/
	install -m 755 update.sh $(DESTDIR)$(PREFIX)/share/avcui/ || true
	@echo "Installed to $(DESTDIR)$(PREFIX)/bin/avcui"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/avcui
	rm -rf $(DESTDIR)$(PREFIX)/share/avcui
	@echo "Uninstalled"

# ─── Dependencies ───────────────────────────────────────────────────────────────
$(OBJ_DIR)/app.o:       $(SRC_DIR)/app.cpp       $(INC_DIR)/app.h $(INC_DIR)/types.h $(INC_DIR)/tui.h $(INC_DIR)/pornhub.h $(INC_DIR)/player.h $(INC_DIR)/input.h $(INC_DIR)/config.h $(INC_DIR)/library.h $(INC_DIR)/log.h $(INC_DIR)/thumbs.h $(INC_DIR)/auth.h $(INC_DIR)/theme.h $(INC_DIR)/provider.h $(INC_DIR)/missav.h $(INC_DIR)/http.h $(INC_DIR)/hls_proxy.h
$(OBJ_DIR)/config.o:    $(SRC_DIR)/config.cpp    $(INC_DIR)/config.h $(INC_DIR)/theme.h
$(OBJ_DIR)/input.o:     $(SRC_DIR)/input.cpp     $(INC_DIR)/input.h $(INC_DIR)/types.h
$(OBJ_DIR)/main.o:      $(SRC_DIR)/main.cpp      $(INC_DIR)/missav.h $(INC_DIR)/http.h $(INC_DIR)/app.h $(INC_DIR)/log.h $(INC_DIR)/player.h $(INC_DIR)/pornhub.h $(INC_DIR)/types.h $(INC_DIR)/theme.h
$(OBJ_DIR)/player.o:    $(SRC_DIR)/player.cpp    $(INC_DIR)/player.h $(INC_DIR)/compat.h $(INC_DIR)/types.h $(INC_DIR)/log.h
$(OBJ_DIR)/tui.o:       $(SRC_DIR)/tui.cpp       $(INC_DIR)/tui.h $(INC_DIR)/types.h $(INC_DIR)/library.h $(INC_DIR)/thumbs.h $(INC_DIR)/theme.h
$(OBJ_DIR)/pornhub.o: $(SRC_DIR)/pornhub.cpp $(INC_DIR)/pornhub.h $(INC_DIR)/types.h $(INC_DIR)/log.h
$(OBJ_DIR)/missav.o:    $(SRC_DIR)/missav.cpp    $(INC_DIR)/missav.h $(INC_DIR)/http.h $(INC_DIR)/log.h $(INC_DIR)/types.h
$(OBJ_DIR)/hls_proxy.o: $(SRC_DIR)/hls_proxy.cpp $(INC_DIR)/hls_proxy.h $(INC_DIR)/http.h $(INC_DIR)/log.h
