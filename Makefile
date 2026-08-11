PROJECT := kilix-object-detect
COMMAND_NAME := kilix-look
BUILD_DIR ?= build
PREFIX ?= /usr/local
DESTDIR ?=

CC ?= cc
AR ?= ar
INSTALL ?= install

CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Iinclude
WARNINGS := \
	-Wall -Wextra -Wpedantic -Wconversion -Wshadow \
	-Wstrict-prototypes -Wmissing-prototypes -Wformat=2
CFLAGS ?= -O2 -g
override CFLAGS += -std=c11 -fPIC $(WARNINGS)

# Vendored and pinned.  The library needs none of these - it is a
# subprocess and some arithmetic - so they are all below this line, where
# the command is.  The terminal stack comes through kilix-rtsp's own
# closure rather than being pinned twice.
RTSP := third_party/kilix-rtsp
MOTION := third_party/kilix-motion-detect
SOUND := third_party/kilix-sound-detect
KTS := $(RTSP)/third_party/kitty-terminal-session
KFB := $(KTS)/third_party/kitty-framebuffer
KIN := $(KTS)/third_party/kitty-input
KKB := $(KIN)/third_party/kitty_keyboard
SR := $(RTSP)/third_party/soft-raster

CMD_CPPFLAGS := -I$(RTSP)/include -I$(MOTION)/include -I$(SOUND)/include \
	-I$(KTS)/include -I$(KFB)/include -I$(KIN)/include -I$(KKB)/include \
	-I$(SR)/include -Isrc
CMD_LDLIBS := -lm -lpthread -lz

CMD_SOURCES := src/main.c src/kod_app.c src/kod_ui.c
CMD_VENDOR_SOURCES := \
	$(RTSP)/src/krtsp_args.c \
	$(RTSP)/src/krtsp_frame.c \
	$(RTSP)/src/krtsp_source.c \
	$(RTSP)/src/krtsp_paths.c \
	$(RTSP)/src/krtsp_config.c \
	$(RTSP)/src/krtsp_exec.c \
	$(MOTION)/src/kilix_motion_detect.c \
	$(SOUND)/src/kilix_sound_detect.c \
	$(KTS)/src/kitty_terminal_session.c \
	$(KFB)/src/kitty_framebuffer.c \
	$(KIN)/src/kitty_input.c \
	$(KIN)/src/kitty_input_posix.c \
	$(KKB)/src/kitty_keyboard.c \
	$(KKB)/src/kitty_keyboard_posix.c \
	$(SR)/src/soft_raster.c
CMD_OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(CMD_SOURCES))
CMD_VENDOR_OBJECTS := \
	$(patsubst %.c,$(BUILD_DIR)/vendor/%.o,$(notdir $(CMD_VENDOR_SOURCES)))
# Pinned upstream code, built with the conversion warnings off so their
# output cannot bury ours.
VENDOR_CFLAGS := $(CFLAGS) -Wno-conversion -Wno-sign-conversion

LIB_OBJECTS := $(BUILD_DIR)/kilix_object_detect.o
STATIC_LIB := $(BUILD_DIR)/lib$(PROJECT).a
COMMAND := $(BUILD_DIR)/$(COMMAND_NAME)

TESTS := $(BUILD_DIR)/test-regions $(BUILD_DIR)/test-detect

TOOLS := tools/kilix-look-detect

.DEFAULT_GOAL := all
.PHONY: all test sanitize install clean

all: $(COMMAND)

$(BUILD_DIR) $(BUILD_DIR)/vendor:
	mkdir -p $@

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CMD_CPPFLAGS) $(CFLAGS) -c $< -o $@

$(STATIC_LIB): $(LIB_OBJECTS)
	$(AR) rcs $@ $^

vpath %.c $(sort $(dir $(CMD_VENDOR_SOURCES)))

$(BUILD_DIR)/vendor/%.o: %.c | $(BUILD_DIR)/vendor
	$(CC) $(CPPFLAGS) $(CMD_CPPFLAGS) $(VENDOR_CFLAGS) -c $< -o $@

$(COMMAND): $(CMD_OBJECTS) $(STATIC_LIB) $(CMD_VENDOR_OBJECTS) | $(BUILD_DIR)
	@test -f $(RTSP)/include/kilix_rtsp.h || { \
		printf 'submodules missing; run: git submodule update --init --recursive\n' >&2; \
		exit 1; }
	$(CC) $(CPPFLAGS) $(CMD_CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ \
		$(CMD_LDLIBS) -o $@

$(BUILD_DIR)/test-%: tests/test_%.c $(STATIC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ -lm -o $@

test: $(TESTS) $(COMMAND)
	@set -e; for binary in $(TESTS); do \
		printf '\n== %s ==\n' "$$binary"; \
		"$$binary"; \
	done; \
	printf '\n== %s --selftest ==\n' "$(COMMAND)"; \
	$(COMMAND) --selftest; \
	printf '\nall test suites passed\n'

sanitize: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
sanitize: LDFLAGS += -fsanitize=address,undefined
sanitize: clean
	@$(MAKE) --no-print-directory CFLAGS="$(CFLAGS)" LDFLAGS="$(LDFLAGS)" test

install: all
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -m 755 $(COMMAND) $(DESTDIR)$(PREFIX)/bin/
	$(INSTALL) -m 755 $(TOOLS) $(DESTDIR)$(PREFIX)/bin/
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/include
	$(INSTALL) -m 644 include/kilix_object_detect.h $(DESTDIR)$(PREFIX)/include/
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/lib
	$(INSTALL) -m 644 $(STATIC_LIB) $(DESTDIR)$(PREFIX)/lib/

clean:
	rm -rf $(BUILD_DIR)
