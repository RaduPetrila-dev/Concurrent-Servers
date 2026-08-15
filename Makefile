# Concurrent servers in C.
#
# Every .c in src/ except utils.c is a standalone server, linked against utils.o.
# Sources named uv-*.c link against libuv and are built only by `make uv`, so a
# missing libuv never blocks the default build.
#
# Servers follow Eli Bendersky's "Programming concurrent servers" series.
# Build system, benchmarks, and extensions are mine.

CC      ?= gcc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
CFLAGS  += -pthread -MMD -MP
LDFLAGS += -pthread

# libuv installs a shared library alongside the .a, so the rpath lets the binary
# find it at run time without setting LD_LIBRARY_PATH.
UV_LIBS := -luv -Wl,-rpath=/usr/local/lib

SRC_DIR   := src
BUILD_DIR := build

BENCH_DIR := bench

# Shared objects every server links against.
COMMON_SRCS := $(SRC_DIR)/utils.c $(SRC_DIR)/protocol.c
COMMON_OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(COMMON_SRCS))

ALL_SRCS  := $(filter-out $(COMMON_SRCS),$(wildcard $(SRC_DIR)/*.c))
UV_SRCS   := $(filter $(SRC_DIR)/uv_%.c,$(ALL_SRCS))
CORE_SRCS := $(filter-out $(UV_SRCS),$(ALL_SRCS))

CORE_BINS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%,$(CORE_SRCS))
UV_BINS   := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%,$(UV_SRCS))
BENCH_BINS := $(BUILD_DIR)/loadgen
DEPS      := $(CORE_BINS:=.d) $(UV_BINS:=.d) $(BENCH_BINS:=.d) \
             $(COMMON_OBJS:.o=.d)

# Without this, make treats the shared objects as intermediate files and
# deletes them after every build, forcing a recompile of utils.c and protocol.c
# each time any single server changes.
.SECONDARY: $(COMMON_OBJS)

.PHONY: all
all: $(CORE_BINS) $(BENCH_BINS)

.PHONY: uv
uv: $(UV_BINS)

.PHONY: everything
everything: all uv

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# The load generator is a standalone client; it shares nothing with the servers.
$(BUILD_DIR)/loadgen: $(BENCH_DIR)/loadgen.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

# More specific than the generic rule below, so make picks it for uv-* sources.
$(BUILD_DIR)/uv_%: $(SRC_DIR)/uv_%.c $(COMMON_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(COMMON_OBJS) $(LDFLAGS) $(UV_LIBS) -o $@

$(BUILD_DIR)/%: $(SRC_DIR)/%.c $(COMMON_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(COMMON_OBJS) $(LDFLAGS) -o $@

# Warnings become errors. CI runs this so a warning cannot reach main.
.PHONY: strict
strict:
	$(MAKE) clean
	$(MAKE) all CFLAGS="$(CFLAGS) -Werror"

# ASan and UBSan. Slow, and it catches the memory bugs a socket server hides
# until it is under load.
.PHONY: debug
debug:
	$(MAKE) clean
	$(MAKE) all \
		CFLAGS="$(CFLAGS) -g3 -O0 -fno-omit-frame-pointer -fsanitize=address,undefined" \
		LDFLAGS="$(LDFLAGS) -fsanitize=address,undefined"

# Benchmark numbers come from this target and no other.
.PHONY: release
release:
	$(MAKE) clean
	$(MAKE) all CFLAGS="$(CFLAGS) -O2 -DNDEBUG"

.PHONY: format
format:
	clang-format -style=file -i $(SRC_DIR)/*.c $(SRC_DIR)/*.h $(BENCH_DIR)/*.c

.PHONY: bench
bench:
	$(MAKE) release
	$(BENCH_DIR)/run_bench.sh
	python3 $(BENCH_DIR)/plot_results.py results/latest

.PHONY: clean
clean:
	@rm -rf $(BUILD_DIR)

.PHONY: help
help:
	@echo "Targets:"
	@echo "  all         build the servers into $(BUILD_DIR)/ (default)"
	@echo "  uv          build the libuv servers, needs libuv installed"
	@echo "  everything  all + uv"
	@echo "  strict      rebuild with -Werror"
	@echo "  debug       rebuild with ASan and UBSan, no optimisation"
	@echo "  release     rebuild with -O2, use this for benchmarks"
	@echo "  format      run clang-format over src/"
	@echo "  bench       release build, then run the full sweep"
	@echo "  clean       remove $(BUILD_DIR)/"
	@echo ""
	@echo "Core servers:  $(notdir $(CORE_BINS))"
	@echo "libuv servers: $(notdir $(UV_BINS))"

-include $(DEPS)
