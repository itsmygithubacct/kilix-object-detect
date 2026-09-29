PROJECT := kilix-object-detect
COMMAND_NAME := kilix-look
BUILD_DIR ?= build
PREFIX ?= /usr/local
DESTDIR ?=
F120_PREFIX ?=

CC ?= cc
AR ?= ar
INSTALL ?= install

CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Iinclude
WARNINGS := \
	-Wall -Wextra -Wpedantic -Wconversion -Wshadow \
	-Wstrict-prototypes -Wmissing-prototypes -Wformat=2
CFLAGS ?= -O2 -g
override CFLAGS += -std=c11 -fPIC $(WARNINGS)

# Command dependencies.  The library needs none of these - it is a subprocess
# and some arithmetic - so they are all below this line, where the command is.
# The terminal stack comes through kilix-rtsp's own closure rather than being
# pinned twice.  Motion detection comes from an immutable F120 public prefix.
RTSP := third_party/kilix-rtsp
SOUND := third_party/kilix-sound-detect
KTS := $(RTSP)/third_party/kitty-terminal-session
KFB := $(KTS)/third_party/kitty-framebuffer
KIN := $(KTS)/third_party/kitty-input
KKB := $(KIN)/third_party/kitty_keyboard
SR := $(RTSP)/third_party/soft-raster

# An explicit F120_PREFIX is used as given and must already hold the header
# and archive. Without one - a catalog or first-use build, where nothing has
# staged the provider - the pinned submodule is staged into the same layout
# under the build directory, so the command still links only public F120
# outputs.
MOTION := third_party/kilix-motion-detect
MOTION_STAGED := $(strip $(F120_PREFIX))
ifeq ($(MOTION_STAGED),)
F120_PREFIX := $(abspath $(BUILD_DIR))/f120-motion
endif
MOTION_CPPFLAGS := -I$(F120_PREFIX)/include
MOTION_HEADER := $(F120_PREFIX)/include/kilix_motion_detect.h
MOTION_LINK_INPUTS := $(F120_PREFIX)/lib/libkilix-motion-detect.a
ifneq ($(MAKECMDGOALS),clean)
ifneq ($(MOTION_STAGED),)
ifeq ($(wildcard $(MOTION_HEADER)),)
$(error F120 public header is missing: $(MOTION_HEADER))
endif
ifeq ($(wildcard $(MOTION_LINK_INPUTS)),)
$(error F120 static archive is missing: $(MOTION_LINK_INPUTS))
endif
endif
endif

CMD_CPPFLAGS := -I$(RTSP)/include $(MOTION_CPPFLAGS) -I$(SOUND)/include \
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

TESTS := $(BUILD_DIR)/test-regions $(BUILD_DIR)/test-detect \
	$(BUILD_DIR)/test-scaler

TOOLS := tools/kilix-look-detect

.DEFAULT_GOAL := all
.PHONY: all test sanitize install clean

all: $(COMMAND)

$(BUILD_DIR) $(BUILD_DIR)/vendor:
	mkdir -p $@

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CMD_CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(CMD_OBJECTS): $(MOTION_HEADER)

ifeq ($(MOTION_STAGED),)
# Build output stays under our build directory so the submodule stays clean.
$(MOTION_HEADER) $(MOTION_LINK_INPUTS) &:
	@test -f $(MOTION)/include/kilix_motion_detect.h || { \
		printf 'submodules missing; run: git submodule update --init --recursive\n' >&2; \
		exit 1; }
	$(MAKE) --no-print-directory -C $(MOTION) \
		BUILD_DIR=$(abspath $(BUILD_DIR))/motion-build \
		PREFIX=$(F120_PREFIX) install
endif

$(STATIC_LIB): $(LIB_OBJECTS)
	$(AR) rcs $@ $^

vpath %.c $(sort $(dir $(CMD_VENDOR_SOURCES)))

$(BUILD_DIR)/vendor/%.o: %.c | $(BUILD_DIR)/vendor
	$(CC) $(CPPFLAGS) $(CMD_CPPFLAGS) $(VENDOR_CFLAGS) -MMD -MP -c $< -o $@

$(COMMAND): $(CMD_OBJECTS) $(STATIC_LIB) $(CMD_VENDOR_OBJECTS) \
	$(MOTION_LINK_INPUTS) | $(BUILD_DIR)
	@test -f $(RTSP)/include/kilix_rtsp.h || { \
		printf 'submodules missing; run: git submodule update --init --recursive\n' >&2; \
		exit 1; }
	$(CC) $(CPPFLAGS) $(CMD_CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ \
		$(CMD_LDLIBS) -o $@

$(BUILD_DIR)/test-%: tests/test_%.c $(STATIC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -DKOD_TEST_BUILD_DIR='"$(BUILD_DIR)"' \
		$(CFLAGS) $(LDFLAGS) -MMD -MP $^ -lm -o $@

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

# What the compiler recorded each object actually included, so editing a
# header rebuilds its users instead of leaving stale objects for the
# tests to measure.
-include $(CMD_OBJECTS:.o=.d) $(CMD_VENDOR_OBJECTS:.o=.d) \
	$(LIB_OBJECTS:.o=.d) $(TESTS:=.d)
