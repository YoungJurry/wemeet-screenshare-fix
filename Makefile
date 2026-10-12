BUILD_DIR ?= /tmp/wemeet-pw-guard-build
PREFIX ?= /usr
LIBDIR ?= $(PREFIX)/lib/wemeet-screenshare-fix
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
CC ?= cc
PYTHON ?= python3
CPPFLAGS += $(shell pkg-config --cflags libpipewire-0.3)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -fPIC -Wall -Wextra -Werror -Wformat=2 -Wshadow
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -fPIC -fvisibility=hidden -fvisibility-inlines-hidden -Wall -Wextra -Werror
LDFLAGS += -Wl,-z,defs -Wl,-z,now

GUARD := $(BUILD_DIR)/libwemeet_pw_guard.so
LAYOUT_GUARD := $(BUILD_DIR)/libwemeet_layout_guard.so
LAYOUT_TEST := $(BUILD_DIR)/test_layout_guard
LAYOUT_PRELOAD_TEST := $(BUILD_DIR)/test_layout_preload
MOCK := $(BUILD_DIR)/libmockpipewire.so
TEST := $(BUILD_DIR)/test_guard

.PHONY: all test test-tooltip check install clean

all: $(GUARD) $(LAYOUT_GUARD)

$(BUILD_DIR):
	mkdir -p $@

$(GUARD): src/guard.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -shared $(LDFLAGS) -o $@ $< -ldl -pthread

$(LAYOUT_GUARD): src/layout_guard.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -shared $(LDFLAGS) -o $@ $< -ldl

$(LAYOUT_TEST): tests/test_layout_guard.cpp src/layout_guard.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< -ldl

$(LAYOUT_PRELOAD_TEST): tests/test_layout_preload.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -Wl,--export-dynamic-symbol=dladdr -o $@ $< -ldl

$(MOCK): tests/mock_pipewire.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -shared $(LDFLAGS) -o $@ $<

$(TEST): tests/test_guard.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< -ldl -pthread

check: $(GUARD) $(LAYOUT_GUARD)
	readelf -h $(GUARD) >/dev/null
	readelf -Ws $(GUARD) | grep -E '[[:space:]]dlsym$$'
	WEMEET_PW_GUARD=0 LD_PRELOAD=$(GUARD) /usr/bin/true
	WEMEET_TOOLTIP_FIX=0 LD_PRELOAD=$(LAYOUT_GUARD) /usr/bin/true

test-tooltip: $(LAYOUT_TEST) $(LAYOUT_PRELOAD_TEST) $(LAYOUT_GUARD)
	PYTHONDONTWRITEBYTECODE=1 $(PYTHON) tests/test_tooltip_patch.py -v
	$(LAYOUT_TEST)
	LD_PRELOAD=$(LAYOUT_GUARD) $(LAYOUT_PRELOAD_TEST)

test: check $(MOCK) $(TEST) test-tooltip
	WEMEET_PW_GUARD=1 LD_PRELOAD=$(GUARD) $(TEST) $(MOCK)

install: $(GUARD) $(LAYOUT_GUARD)
	install -Dm755 "$(GUARD)" "$(DESTDIR)$(LIBDIR)/libwemeet_pw_guard.so"
	install -Dm755 "$(LAYOUT_GUARD)" "$(DESTDIR)$(LIBDIR)/libwemeet_layout_guard.so"
	install -Dm755 tools/stage_hunk0.py "$(DESTDIR)$(LIBDIR)/stage_hunk0.py"
	install -Dm755 tools/stage_tooltip.py "$(DESTDIR)$(LIBDIR)/stage_tooltip.py"
	install -Dm755 tools/run-guarded.sh "$(DESTDIR)$(BINDIR)/wemeet-wayland-guard"
	install -Dm644 data/wemeet-wayland-guard.desktop "$(DESTDIR)$(DATADIR)/applications/wemeet-wayland-guard.desktop"
	install -Dm644 LICENSE "$(DESTDIR)$(DATADIR)/licenses/wemeet-screenshare-fix/LICENSE"
	install -Dm644 README.md "$(DESTDIR)$(DATADIR)/doc/wemeet-screenshare-fix/README.md"
	install -Dm644 docs/tooltip-reentrancy.md "$(DESTDIR)$(DATADIR)/doc/wemeet-screenshare-fix/tooltip-reentrancy.md"

clean:
	rm -rf $(BUILD_DIR)
