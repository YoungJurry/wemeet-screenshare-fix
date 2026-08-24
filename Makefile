BUILD_DIR ?= /tmp/wemeet-pw-guard-build
CC ?= cc
CPPFLAGS += $(shell pkg-config --cflags libpipewire-0.3)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -fPIC -Wall -Wextra -Werror -Wformat=2 -Wshadow
LDFLAGS += -Wl,-z,defs -Wl,-z,now

GUARD := $(BUILD_DIR)/libwemeet_pw_guard.so
MOCK := $(BUILD_DIR)/libmockpipewire.so
TEST := $(BUILD_DIR)/test_guard

.PHONY: all test check clean

all: $(GUARD)

$(BUILD_DIR):
	mkdir -p $@

$(GUARD): src/guard.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -shared $(LDFLAGS) -o $@ $< -ldl -pthread

$(MOCK): tests/mock_pipewire.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -shared $(LDFLAGS) -o $@ $<

$(TEST): tests/test_guard.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< -ldl -pthread

check: $(GUARD)
	readelf -h $(GUARD) >/dev/null
	readelf -Ws $(GUARD) | grep -E '[[:space:]]dlsym$$'
	WEMEET_PW_GUARD=0 LD_PRELOAD=$(GUARD) /usr/bin/true

test: check $(MOCK) $(TEST)
	WEMEET_PW_GUARD=1 LD_PRELOAD=$(GUARD) $(TEST) $(MOCK)

clean:
	rm -rf $(BUILD_DIR)
