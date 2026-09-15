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
UV_LIBS   := -luv -Wl,-rpath=/usr/local/lib
URING_LIBS := -luring

SRC_DIR   := src
BUILD_DIR := build

BENCH_DIR := bench
TEST_DIR  := tests

# Shared objects every server links against. metrics.c and metrics_server.c
# belong here rather than in ALL_SRCS: they have no main, so the generic binary
# rule would try to link them as standalone servers and fail.
COMMON_SRCS := $(SRC_DIR)/utils.c $(SRC_DIR)/protocol.c $(SRC_DIR)/peer_state.c \
               $(SRC_DIR)/metrics.c $(SRC_DIR)/metrics_server.c
COMMON_OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(COMMON_SRCS))

ALL_SRCS  := $(filter-out $(COMMON_SRCS),$(wildcard $(SRC_DIR)/*.c))
UV_SRCS    := $(filter $(SRC_DIR)/uv_%.c,$(ALL_SRCS))
URING_SRCS := $(filter $(SRC_DIR)/uring_%.c,$(ALL_SRCS))
CORE_SRCS  := $(filter-out $(UV_SRCS) $(URING_SRCS),$(ALL_SRCS))

CORE_BINS  := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%,$(CORE_SRCS))
UV_BINS    := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%,$(UV_SRCS))
URING_BINS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%,$(URING_SRCS))
BENCH_BINS := $(BUILD_DIR)/loadgen
TEST_BINS  := $(BUILD_DIR)/test_metrics
DEPS := $(CORE_BINS:=.d) $(UV_BINS:=.d) $(URING_BINS:=.d) $(BENCH_BINS:=.d) \
        $(TEST_BINS:=.d) $(COMMON_OBJS:.o=.d)

# Everything clang-format touches. Wildcards keep an empty directory from
# breaking the target.
FORMAT_SRCS := $(wildcard $(SRC_DIR)/*.c) $(wildcard $(SRC_DIR)/*.h) \
               $(wildcard $(BENCH_DIR)/*.c) $(wildcard $(TEST_DIR)/*.c)

# Without this, make treats the shared objects as intermediate files and
# deletes them after every build, forcing a recompile of utils.c and protocol.c
# each time any single server changes.
.SECONDARY: $(COMMON_OBJS)

.PHONY: all
all: $(CORE_BINS) $(BENCH_BINS)

.PHONY: uv
uv: $(UV_BINS)

.PHONY: uring
uring: $(URING_BINS)

.PHONY: everything
everything: all uv uring

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# The load generator is a standalone client; it shares nothing with the servers.
$(BUILD_DIR)/loadgen: $(BENCH_DIR)/loadgen.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

# The metrics tests link metrics.c directly so a sanitiser run of the tests
# stays independent of whatever flags the servers were built with.
$(BUILD_DIR)/test_metrics: $(TEST_DIR)/test_metrics.c $(SRC_DIR)/metrics.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(SRC_DIR) $^ $(LDFLAGS) -o $@

# More specific than the generic rule below, so make picks it for uv-* sources.
$(BUILD_DIR)/uv_%: $(SRC_DIR)/uv_%.c $(COMMON_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(COMMON_OBJS) $(LDFLAGS) $(UV_LIBS) -o $@

$(BUILD_DIR)/uring_%: $(SRC_DIR)/uring_%.c $(COMMON_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(COMMON_OBJS) $(LDFLAGS) $(URING_LIBS) -o $@

$(BUILD_DIR)/%: $(SRC_DIR)/%.c $(COMMON_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(COMMON_OBJS) $(LDFLAGS) -o $@

.PHONY: test
test: $(TEST_BINS)
	$(BUILD_DIR)/test_metrics

# Warnings become errors. CI runs this so a warning cannot reach main.
# libuv and io_uring are included when present, matching what release does.
.PHONY: strict
strict:
	$(MAKE) clean
	$(MAKE) all CFLAGS="$(CFLAGS) -Werror"
	@if pkg-config --exists libuv 2>/dev/null; then \
		$(MAKE) uv CFLAGS="$(CFLAGS) -Werror"; \
	else \
		echo "libuv not found, skipping uv servers"; \
	fi
	@if pkg-config --exists liburing 2>/dev/null; then \
		$(MAKE) uring CFLAGS="$(CFLAGS) -Werror"; \
	else \
		echo "liburing not found, skipping uring_server"; \
	fi

# ASan and UBSan. Slow, and it catches the memory bugs a socket server hides
# until it is under load.
.PHONY: debug
debug:
	$(MAKE) clean
	$(MAKE) all \
		CFLAGS="$(CFLAGS) -g3 -O0 -fno-omit-frame-pointer -fsanitize=address,undefined" \
		LDFLAGS="$(LDFLAGS) -fsanitize=address,undefined"

# ThreadSanitizer. The metrics counters are lock free, so this is the target
# that proves the atomics are right.
.PHONY: tsan
tsan:
	$(MAKE) clean
	$(MAKE) all \
		CFLAGS="$(CFLAGS) -g3 -O1 -fno-omit-frame-pointer -fsanitize=thread" \
		LDFLAGS="$(LDFLAGS) -fsanitize=thread"

.PHONY: test-debug
test-debug:
	$(MAKE) clean
	$(MAKE) test \
		CFLAGS="$(CFLAGS) -g3 -O0 -fno-omit-frame-pointer -fsanitize=address,undefined" \
		LDFLAGS="$(LDFLAGS) -fsanitize=address,undefined"

.PHONY: test-tsan
test-tsan:
	$(MAKE) clean
	$(MAKE) test \
		CFLAGS="$(CFLAGS) -g3 -O1 -fno-omit-frame-pointer -fsanitize=thread" \
		LDFLAGS="$(LDFLAGS) -fsanitize=thread"

# Benchmark numbers come from this target and no other. io_uring is included
# when liburing is present, since run_bench.sh sweeps it alongside the rest.
.PHONY: release
release:
	$(MAKE) clean
	$(MAKE) all CFLAGS="$(CFLAGS) -O2 -DNDEBUG"
	@if pkg-config --exists liburing 2>/dev/null; then \
		$(MAKE) uring CFLAGS="$(CFLAGS) -O2 -DNDEBUG"; \
	else \
		echo "liburing not found, skipping uring_server"; \
	fi

.PHONY: format
format:
	clang-format -style=file -i $(FORMAT_SRCS)

# Same file set as format, but fails instead of rewriting. This is what CI runs.
.PHONY: format-check
format-check:
	clang-format -style=file --dry-run --Werror $(FORMAT_SRCS)

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
	@echo "  all          build the servers into $(BUILD_DIR)/ (default)"
	@echo "  uv           build the libuv servers, needs libuv installed"
	@echo "  uring        build the io_uring server, needs liburing installed"
	@echo "  everything   all + uv + uring"
	@echo "  test         build and run the metrics unit tests"
	@echo "  test-debug   run the metrics tests under ASan and UBSan"
	@echo "  test-tsan    run the metrics tests under ThreadSanitizer"
	@echo "  strict       rebuild with -Werror"
	@echo "  debug        rebuild with ASan and UBSan, no optimisation"
	@echo "  tsan         rebuild with ThreadSanitizer"
	@echo "  release      rebuild with -O2, use this for benchmarks"
	@echo "  format       run clang-format over src/, bench/ and tests/"
	@echo "  format-check same file set, fails instead of rewriting"
	@echo "  bench        release build, then run the full sweep"
	@echo "  clean        remove $(BUILD_DIR)/"
	@echo ""
	@echo "Core servers:  $(notdir $(CORE_BINS))"
	@echo "libuv servers: $(notdir $(UV_BINS))"
	@echo "io_uring:      $(notdir $(URING_BINS))"

-include $(DEPS)
