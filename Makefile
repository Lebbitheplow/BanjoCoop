# BanjoCoop.
#
#   make            build the MIPS mod (.nrm) and the native transport (.so)
#   make test-native  run the headless transport tests (no ROM/game needed)
#   make install    place both artifacts in the game's mods directory
#   make run        install and launch the game
#
# See docs/symbols.md for toolchain notes and gotchas.

BUILD_DIR  := build
TEMPLATE   := vendor/BKRecompModTemplate
MOD_TOOL   := tools/RecompModTool
MOD_TOML   := config/mod.toml

CC := clang
LD := ld.lld

TARGET   := $(BUILD_DIR)/mod.elf
LDSCRIPT := $(TEMPLATE)/mod.ld

NATIVE_BUILD := build-native
NATIVE_LIB   := $(NATIVE_BUILD)/banjocoop_net.so

ARCHFLAGS := -target mips -mips2 -mabi=32 -O2 -G0 -mno-abicalls -mno-odd-spreg -mno-check-zero-division \
             -fomit-frame-pointer -ffast-math -fno-unsafe-math-optimizations -fno-builtin-memset \
             -funsigned-char -fno-builtin-sinf -fno-builtin-cosf
WARNFLAGS := -Wall -Wextra -Wno-incompatible-library-redeclaration -Wno-unused-parameter \
             -Wno-unknown-pragmas -Wno-unused-variable -Wno-missing-braces \
             -Wno-unsupported-floating-point-opt -Werror=section -Wno-visibility
CFLAGS    := $(ARCHFLAGS) $(WARNFLAGS) -D_LANGUAGE_C -nostdinc -ffunction-sections
CPPFLAGS  := -nostdinc -DMIPS -DF3DEX_GBI \
             -I src/native/include \
             -I $(TEMPLATE)/include -I $(TEMPLATE)/include/dummy_headers \
             -I $(TEMPLATE)/bk-decomp/include -I $(TEMPLATE)/bk-decomp/include/2.0L \
             -I $(TEMPLATE)/bk-decomp/include/2.0L/PR
LDFLAGS   := -nostdlib -T $(LDSCRIPT) -Map $(BUILD_DIR)/mod.map --unresolved-symbols=ignore-all \
             --emit-relocs -e 0 --no-nmagic -gc-sections

rwildcard = $(foreach d,$(wildcard $(1:=/*)),$(call rwildcard,$d,$2) $(filter $(subst *,%,$2),$d))
getdirs   = $(sort $(dir $(1)))

C_SRCS := $(call rwildcard,src/mod,*.c)
C_OBJS := $(addprefix $(BUILD_DIR)/, $(C_SRCS:.c=.o))
C_DEPS := $(addprefix $(BUILD_DIR)/, $(C_SRCS:.c=.d))

BUILD_DIRS := $(call getdirs,$(C_OBJS))

# The release build ignores the portable.txt marker and always uses the XDG config dir.
# BANJO_CONFIG is overridable so two instances can run against separate config dirs:
#   make run BANJO_CONFIG=$HOME/.config/BanjoRecompiled-p2
BANJO_CONFIG ?= $(HOME)/.config/BanjoRecompiled
MODS_DIR     := $(BANJO_CONFIG)/mods

# The prebuilt release needs SDL >= 2.26 (SDL_GetWindowSizeInPixels); Ubuntu 22.04 ships 2.0.20.
# gamescope bundles 2.26.3 built against system libs, so borrow it rather than installing anything.
RUNTIME    := runtime
SDL_COMPAT := /opt/gamescope/lib/x86_64-linux-gnu

all: $(BUILD_DIR)/banjocoop.nrm native

# The native transport builds with the system compiler and shares only protocol.h with the mod.
native:
	@cmake -S src/native -B $(NATIVE_BUILD) -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null
	@cmake --build $(NATIVE_BUILD) -j$$(nproc)

test-native: native
	./$(NATIVE_BUILD)/bcnet_test

$(BUILD_DIR)/banjocoop.nrm: $(TARGET) $(MOD_TOML)
	$(MOD_TOOL) $(MOD_TOML) $(BUILD_DIR)

$(TARGET): $(C_OBJS) $(LDSCRIPT) | $(BUILD_DIR)
	$(LD) $(C_OBJS) $(LDFLAGS) -o $@

$(BUILD_DIR) $(BUILD_DIRS):
	mkdir -p $@

$(C_OBJS): $(BUILD_DIR)/%.o : %.c | $(BUILD_DIRS)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -MMD -MF $(@:.o=.d) -c -o $@

# The .so goes NEXT TO the .nrm, not inside it — the runtime resolves it from the mod file's
# parent directory, appending the platform library extension.
install: $(BUILD_DIR)/banjocoop.nrm native
	mkdir -p $(MODS_DIR)
	cp $(BUILD_DIR)/banjocoop.nrm $(MODS_DIR)/
	cp $(NATIVE_LIB) $(MODS_DIR)/

# Output is teed to a log as well as the terminal. Without this the only record of a session is
# whatever scrolled past in somebody's terminal, which makes diagnosing a sync problem a game of
# telephone. `make diag` reads these back.
LOG_DIR := $(CURDIR)/run/logs

run: install
	@mkdir -p $(LOG_DIR)
	cd $(RUNTIME) && LD_LIBRARY_PATH=$(SDL_COMPAT) ./BanjoRecompiled 2>&1 | tee $(LOG_DIR)/p1.log

# --- second instance ----------------------------------------------------------------------
# The runtime resolves its config dir from HOME and ignores both XDG_CONFIG_HOME and the
# portable.txt marker (both verified), so a HOME override is the way to get a second instance
# with independent settings.
#
# This deliberately lives OUTSIDE $(BUILD_DIR): it holds the second player's save file and
# settings, and having it under build/ meant `make clean` silently destroyed them — costing a
# re-import of the ROM and, far worse, sitting through the intro again to make a new file.
# Nothing here is a build artifact, so nothing here is `clean`'s business.
INSTANCE2     := $(CURDIR)/run/instance2
INSTANCE2_CFG := $(INSTANCE2)/.config/BanjoRecompiled
SAVE_NAME     := bk.n64.us.1.0.bin
SNAPSHOT_DIR  := $(CURDIR)/run/snapshots

install-p2: $(BUILD_DIR)/banjocoop.nrm native
	@mkdir -p $(INSTANCE2_CFG)/mods
	@cp $(BUILD_DIR)/banjocoop.nrm $(INSTANCE2_CFG)/mods/
	@cp $(NATIVE_LIB) $(INSTANCE2_CFG)/mods/
	@python3 scripts/setup_instance2.py "$(BANJO_CONFIG)" "$(INSTANCE2_CFG)"

run-p2: install-p2
	@mkdir -p $(LOG_DIR)
	cd $(RUNTIME) && HOME=$(INSTANCE2) LD_LIBRARY_PATH=$(SDL_COMPAT) ./BanjoRecompiled 2>&1 | tee $(LOG_DIR)/p2.log

# --- save snapshots -------------------------------------------------------------------------
# Testing late-join and shared-progress behaviour means repeatedly putting a player back to a
# known save state. Doing that by deleting the save costs the whole intro and file-creation
# sequence every single time. Snapshot once, restore instantly thereafter.
#
#   make snapshot-p2   after getting player 2 to the state you want to keep returning to
#   make restore-p2    to go back to it, in about a millisecond
#
# Snapshot a file that has already been created and had the intro watched, and restoring never
# replays the intro.
snapshot-p2:
	@mkdir -p $(SNAPSHOT_DIR)
	@if [ ! -f "$(INSTANCE2_CFG)/saves/$(SAVE_NAME)" ]; then \
		echo "no player 2 save yet - run the game first"; exit 1; fi
	@cp "$(INSTANCE2_CFG)/saves/$(SAVE_NAME)" "$(SNAPSHOT_DIR)/p2-baseline.bin"
	@echo "saved player 2 baseline -> $(SNAPSHOT_DIR)/p2-baseline.bin"

restore-p2:
	@if [ ! -f "$(SNAPSHOT_DIR)/p2-baseline.bin" ]; then \
		echo "no baseline - run 'make snapshot-p2' once first"; exit 1; fi
	@mkdir -p $(INSTANCE2_CFG)/saves
	@cp "$(SNAPSHOT_DIR)/p2-baseline.bin" "$(INSTANCE2_CFG)/saves/$(SAVE_NAME)"
	@rm -f "$(INSTANCE2_CFG)/saves/$(SAVE_NAME).bak"
	@echo "player 2 restored to baseline"

snapshot-p1:
	@mkdir -p $(SNAPSHOT_DIR)
	@if [ ! -f "$(BANJO_CONFIG)/saves/$(SAVE_NAME)" ]; then \
		echo "no player 1 save yet - run the game first"; exit 1; fi
	@cp "$(BANJO_CONFIG)/saves/$(SAVE_NAME)" "$(SNAPSHOT_DIR)/p1-baseline.bin"
	@echo "saved player 1 baseline -> $(SNAPSHOT_DIR)/p1-baseline.bin"

restore-p1:
	@if [ ! -f "$(SNAPSHOT_DIR)/p1-baseline.bin" ]; then \
		echo "no baseline - run 'make snapshot-p1' once first"; exit 1; fi
	@mkdir -p $(BANJO_CONFIG)/saves
	@cp "$(SNAPSHOT_DIR)/p1-baseline.bin" "$(BANJO_CONFIG)/saves/$(SAVE_NAME)"
	@rm -f "$(BANJO_CONFIG)/saves/$(SAVE_NAME).bak"
	@echo "player 1 restored to baseline"

# --- crash debugging --------------------------------------------------------------------------
# Runs under gdb in batch mode: the game behaves normally, but on a segfault gdb prints a
# backtrace and register dump to a log instead of the process just vanishing with code 139.
# Core dumps are unavailable here (ulimit -c is 0 and apport owns core_pattern), so this is the
# way to get a stack out of a crash.
DEBUG_LOG    := $(CURDIR)/$(BUILD_DIR)/crash-p1.log
DEBUG_LOG_P2 := $(CURDIR)/$(BUILD_DIR)/crash-p2.log

GDB_ARGS = -batch -ex "handle SIGPIPE nostop noprint pass" -ex run \
           -ex "echo \n===== BACKTRACE =====\n" -ex "bt 40" \
           -ex "echo \n===== THREADS =====\n" -ex "thread apply all bt 15"

debug: install
	cd $(RUNTIME) && LD_LIBRARY_PATH=$(SDL_COMPAT) \
		gdb $(GDB_ARGS) --args ./BanjoRecompiled 2>&1 | tee $(DEBUG_LOG)
	@echo "--- saved to $(DEBUG_LOG) ---"

debug-p2: install-p2
	cd $(RUNTIME) && HOME=$(INSTANCE2) LD_LIBRARY_PATH=$(SDL_COMPAT) \
		gdb $(GDB_ARGS) --args ./BanjoRecompiled 2>&1 | tee $(DEBUG_LOG_P2)
	@echo "--- saved to $(DEBUG_LOG_P2) ---"

# --- import check -----------------------------------------------------------------------------
# vendor/BanjoRecomp is a clone of main; runtime/BanjoRecompiled is the 1.0.1 release. main has
# mod-facing exports that 1.0.1 does not, so code written against the vendored source can compile
# and package perfectly and then fail at load with "Imported function not found".
#
# This compares what the built mod actually imports from the base recomp against what the runtime
# we ship alongside can provide. Run it before trusting a build.
check-imports: $(TARGET)
	@sec=$$(readelf -W -S $(TARGET) | grep -oE '^ *\[ *[0-9]+\] \.recomp_import\.\*' | grep -oE '[0-9]+' | head -1); \
	if [ -z "$$sec" ]; then echo "no base-recomp imports"; exit 0; fi; \
	missing=0; \
	for sym in $$(readelf -W -s $(TARGET) | awk -v s="$$sec" '$$7==s {print $$8}' | grep -v '^\.' | sort -u); do \
		if strings -a $(RUNTIME)/BanjoRecompiled | grep -qxF "$$sym"; then \
			echo "  ok      $$sym"; \
		else \
			echo "  MISSING $$sym"; missing=1; \
		fi; \
	done; \
	if [ $$missing -ne 0 ]; then \
		echo "*** the runtime cannot resolve every import - the mod will fail to load ***"; exit 1; \
	fi

# Build artifacts only. run/ holds the second player's save, settings and snapshots and is
# deliberately not touched here — see the second-instance section above for why.
# Reads back the last session from both instances and answers the questions that actually matter,
# in order, so a sync problem can be diagnosed from evidence instead of from a retest.
diag:
	@for who in p1 p2; do \
		f=$(LOG_DIR)/$$who.log; \
		echo "===== $$who ====="; \
		if [ ! -f $$f ]; then echo "  no log - run 'make run' / 'make run-p2'"; continue; fi; \
		echo "-- did the mod load and start?"; grep -a "banjocoop\] init\|hosting\|joining\|connected as\|rejected\|failed" $$f | tail -6 || true; \
		echo "-- progression mirror"; grep -a "progression mirror\|publishing progression" $$f | tail -6 || true; \
		echo "-- object send rates (only the peer owning a map prints these)"; grep -a "objtier" $$f | tail -6 || true; \
		echo "-- errors"; grep -aiE "not found|failed to load|abort|error" $$f | tail -6 || true; \
	done
	@echo "====="
	@echo "expected on a working session:"
	@echo "  p1: 'hosting on port'  +  'publishing progression mirror: N bits set'"
	@echo "  p2: 'joining' + 'connected as player 1' + 'first progression mirror from host'"
	@echo "  either: 'objtier ... sent=N/16' from whoever owns the map they are standing in."
	@echo "    culled climbing while enemies are on screen => BC_TIER_FAR too small (src/mod/tier.h)"
	@echo "    nothing ever culled in a full map          => all three bands too large"

# Compiles the real src/mod/progress.c against fake game accessors. The mod's code only loads when
# a game session starts, so this is the only way to check its logic without playing the game.
test-modlogic: | $(BUILD_DIR)
	@gcc -std=c11 -Wall -Wextra -I tests/modlogic/stubs -I src/native/include \
		tests/modlogic/test_progress.c tests/modlogic/stubs/progress.c \
		-o $(BUILD_DIR)/test_progress
	@$(BUILD_DIR)/test_progress
# Send-rate tiering. No stubs at all — src/mod/tier.c is written to depend on nothing but
# protocol.h precisely so it can be compiled and checked here rather than only in-game.
	@gcc -std=c11 -Wall -Wextra -I src/mod -I src/native/include \
		tests/modlogic/test_tiers.c src/mod/tier.c \
		-o $(BUILD_DIR)/test_tiers
	@$(BUILD_DIR)/test_tiers

clean:
	rm -rf $(BUILD_DIR) $(NATIVE_BUILD)

-include $(C_DEPS)

.PHONY: all clean install run native test-native test-modlogic install-p2 run-p2 \
        check-imports diag snapshot-p1 restore-p1 snapshot-p2 restore-p2
