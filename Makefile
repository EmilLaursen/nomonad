MAKEFLAGS += --no-builtin-rules
.SUFFIXES:

VERSION ?= 0.1.0
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
DOCDIR  ?= $(PREFIX)/share/doc/nomonad

PKGS    := wayland-client xkbcommon
# -MMD -MP makes the compiler emit a .d file alongside each .o listing
# every header the .c included; we -include those at the bottom so a
# header change retriggers the right .o's. -MP adds phony targets so
# deleting a header does not break the build.
CFLAGS  := -std=c99 -O2 -Wall -Wextra -Wpedantic \
           -Wno-unused-parameter \
           -D_POSIX_C_SOURCE=200809L \
           -DNM_VERSION='"$(VERSION)"' \
           -MMD -MP \
           $(shell pkg-config --cflags $(PKGS)) \
           $(EXTRA_CFLAGS)
LDLIBS  := $(shell pkg-config --libs $(PKGS))
SCANNER := $(shell pkg-config --variable=wayland_scanner wayland-scanner)

# Sanitizer build (`make asan`). ASan catches OOB / use-after-free /
# leaks; UBSan catches signed overflow, null deref, misaligned loads,
# OOB shifts. They compose. -O1 keeps inlining shallow enough to get
# readable reports; frame pointers are needed for the symbolizer.
ifdef ASAN
CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1
LDLIBS += -fsanitize=address,undefined
endif

# Perf-friendly build (`make perf`). Keeps -O2 but adds frame pointers
# and debug info so `perf record --call-graph dwarf` produces honest
# stacks.
ifdef PERF
CFLAGS += -fno-omit-frame-pointer -ggdb3
endif

# libFuzzer build (`make fuzz`). Builds test/harness/fuzz with
# sancov instrumentation everywhere and links the fuzzer runtime in.
# Composed with ASan + UBSan so structural assertions fire as crashes.
# Clang-only; the make target overrides CC accordingly.
ifdef FUZZ
CFLAGS += -fsanitize=fuzzer-no-link,address,undefined \
          -fno-omit-frame-pointer -g -O1
LDLIBS += -fsanitize=fuzzer,address,undefined
endif

ifdef FUZZ
O_DEFAULT := build-fuzz
else ifdef ASAN
O_DEFAULT := build-asan
else ifdef PERF
O_DEFAULT := build-perf
else
O_DEFAULT := build
endif
O ?= $(O_DEFAULT)

PROTOCOLS := river-window-management-v1 river-xkb-bindings-v1 \
             river-layer-shell-v1 river-input-management-v1 \
             river-libinput-config-v1 river-xkb-config-v1
GEN_DIR   := $(O)/gen
GEN_C     := $(addprefix $(GEN_DIR)/,$(addsuffix -protocol.c,$(PROTOCOLS)))
GEN_H     := $(addprefix $(GEN_DIR)/,$(addsuffix -client-protocol.h,$(PROTOCOLS)))

SRC := src/main.c src/binding.c src/sexpr.c src/config.c src/layout.c \
       src/layout_names.c src/action.c src/output.c src/window.c \
       src/seat.c src/wm.c src/recorder.c src/timing.c src/spawnd.c \
       src/status.c src/input.c src/restore.c src/ctl.c \
       src/keymap.c
OBJ := $(addprefix $(O)/,$(SRC:.c=.o)) $(GEN_C:.c=.o)
BIN := $(O)/nomonad

TESTS := $(O)/test/sexpr_test $(O)/test/config_test

# Every object depends on $(O)/.flags, which is rewritten only when the
# compiler, flags or libraries differ from the ones it records. A change
# rebuilds that output directory, and nothing else.
FLAGS_FILE := $(O)/.flags
FLAGS_NOW  := $(CC) $(CFLAGS) $(LDLIBS)
$(shell mkdir -p $(O))
ifneq ($(FLAGS_NOW),$(file < $(FLAGS_FILE)))
$(file > $(FLAGS_FILE),$(FLAGS_NOW))
endif

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $^ $(LDLIBS) -o $@

$(GEN_DIR)/%-protocol.c: protocol/%.xml | $(GEN_DIR)
	$(SCANNER) private-code  $< $@
$(GEN_DIR)/%-client-protocol.h: protocol/%.xml | $(GEN_DIR)
	$(SCANNER) client-header $< $@

$(O)/src/%.o: src/%.c $(GEN_H) $(FLAGS_FILE) | $(O)/src
	$(CC) $(CFLAGS) -Isrc -I$(GEN_DIR) -c $< -o $@

# sexpr and layout_names are libc-only. These rules keep `make test`
# runnable on hosts without wayland-scanner.
$(O)/src/sexpr.o: src/sexpr.c src/sexpr.h $(FLAGS_FILE) | $(O)/src
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

$(O)/src/layout_names.o: src/layout_names.c src/layout.h $(FLAGS_FILE) | $(O)/src
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

$(O)/test/%.o: test/%.c $(FLAGS_FILE) | $(GEN_H) $(O)/test/harness
	$(CC) $(CFLAGS) -Isrc -I$(GEN_DIR) -c $< -o $@

$(GEN_DIR)/%.o: $(GEN_DIR)/%.c $(FLAGS_FILE)
	$(CC) $(CFLAGS) -I$(GEN_DIR) -c $< -o $@

$(GEN_DIR) $(O)/src $(O)/test/harness:
	mkdir -p $@

$(O)/test/sexpr_test: $(O)/test/sexpr_test.o $(O)/src/sexpr.o
	$(CC) $^ $(LDLIBS) -o $@

$(O)/test/config_test: $(O)/test/config_test.o $(O)/src/config.o \
                       $(O)/src/sexpr.o $(O)/src/layout_names.o
	$(CC) $^ $(LDLIBS) -o $@

test: $(TESTS) check-docs
	$(O)/test/sexpr_test
	$(O)/test/config_test

docs: $(BIN) docs/gen_config.py
	$(BIN) --describe | python3 -I docs/gen_config.py > $(O)/config.md
	mv $(O)/config.md docs/config.md

check-docs: $(BIN) docs/gen_config.py
	@$(BIN) --describe | python3 -I docs/gen_config.py | \
		cmp -s - docs/config.md || \
		{ echo "docs/config.md is stale, run make docs"; exit 1; }

# Headless harness: drives the manage / render cycle in process. fakewl.c
# wraps libwayland-client's proxy entry points via -Wl,--wrap so the
# generated river_*_v1_* request inlines become no-ops on fake proxies,
# harness_lib.c provides main.c's globals plus the injection / cycle /
# invariant API, harness.c holds the scenarios. The fuzzer and the
# replayer link the same library.
HARNESS_LIB_OBJ := $(addprefix $(O)/,test/harness/harness_lib.o \
                   test/harness/fakewl.o $(filter-out src/main.o,$(SRC:.c=.o))) \
                   $(GEN_C:.c=.o)
HARNESS_WRAPS := -Wl,--wrap=wl_proxy_marshal_flags \
                 -Wl,--wrap=wl_proxy_destroy \
                 -Wl,--wrap=wl_proxy_get_user_data \
                 -Wl,--wrap=wl_proxy_set_user_data \
                 -Wl,--wrap=wl_proxy_get_version \
                 -Wl,--wrap=wl_proxy_add_listener

$(GEN_DIR)/phase_table.inc: test/harness/gen_phase.py $(wildcard protocol/*.xml) | $(GEN_DIR)
	python3 test/harness/gen_phase.py $(sort $(wildcard protocol/*.xml)) > $@

$(O)/test/harness/fakewl.o: $(GEN_DIR)/phase_table.inc

$(O)/test/harness/harness $(O)/test/harness/fuzz $(O)/test/harness/replay: \
		$(O)/test/harness/%: $(O)/test/harness/%.o $(HARNESS_LIB_OBJ)
	$(CC) $^ $(LDLIBS) $(HARNESS_WRAPS) -o $@

harness: $(O)/test/harness/harness
	$(O)/test/harness/harness

replay: $(O)/test/harness/replay

# Sanitizer, perf and fuzz builds, each in its own output directory
# unless O is given. libFuzzer is clang-only.
asan:
	$(MAKE) all ASAN=1

perf:
	$(MAKE) all PERF=1

fuzz:
	$(MAKE) fuzz-bin FUZZ=1 CC=clang

fuzz-bin: $(O)/test/harness/fuzz

install: $(BIN)
	install -Dm 0755 $(BIN) $(DESTDIR)$(BINDIR)/nomonad
	install -Dm 0644 README.txt LICENSE -t $(DESTDIR)$(DOCDIR)
	install -Dm 0644 examples/*.scm -t $(DESTDIR)$(DOCDIR)/examples
	install -Dm 0644 docs/config.md -t $(DESTDIR)$(DOCDIR)

install-contrib:
	install -Dm 0755 contrib/nomonad-switch contrib/nomonad-keys contrib/nomonad-fzf -t $(DESTDIR)$(BINDIR)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/nomonad $(DESTDIR)$(BINDIR)/nomonad-switch \
	      $(DESTDIR)$(BINDIR)/nomonad-keys $(DESTDIR)$(BINDIR)/nomonad-fzf
	rm -rf $(DESTDIR)$(DOCDIR)

clean:
	rm -rf $(O)

distclean:
	rm -rf build build-* $(O)

-include $(wildcard $(O)/src/*.d $(O)/test/*.d $(O)/test/harness/*.d $(GEN_DIR)/*.d)

# compile_commands.json for clangd: a fresh build of the binary and the
# tests under bear, so files under test/ get their flags recorded too.
compdb:
	$(MAKE) clean
	bear -- $(MAKE) all $(TESTS)

.SECONDARY: $(GEN_C)

.PHONY: all install install-contrib uninstall clean distclean test compdb asan perf harness fuzz fuzz-bin replay docs check-docs
