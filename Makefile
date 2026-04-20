CC ?= gcc

C_STD ?= -std=c11
WARNINGS ?= -Wall -Wextra -Wpedantic
NATIVE_OPT ?= -Ofast -march=native -mtune=native -flto -funroll-loops
THREAD_FLAGS ?= -pthread

PGO_MODE ?= off
PGO_DIR ?= .pgo-data
PGO_GEN_FLAGS ?= -fprofile-generate=$(PGO_DIR)
PGO_USE_FLAGS ?= -fprofile-use=$(PGO_DIR) -fprofile-correction -fprofile-update=prefer-atomic

CPPFLAGS ?= -D_POSIX_C_SOURCE=200809L
CFLAGS ?= $(C_STD) $(WARNINGS) $(NATIVE_OPT) $(THREAD_FLAGS)
LDFLAGS ?= $(THREAD_FLAGS)
LDLIBS ?=

ifeq ($(PGO_MODE),gen)
CFLAGS += $(PGO_GEN_FLAGS)
LDFLAGS += $(PGO_GEN_FLAGS)
endif

ifeq ($(PGO_MODE),use)
CFLAGS += $(PGO_USE_FLAGS)
LDFLAGS += $(PGO_USE_FLAGS)
endif

SRC := $(shell find src -name '*.c' | sort)
OBJ := $(SRC:.c=.o)
BIN := build/chess_bot

.PHONY: all clean clean-pgo pgo-gen-build pgo-train pgo-use-build pgo pgo-gen pgo-use

all: $(BIN)

$(BIN): $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(OBJ) $(LDFLAGS) $(LDLIBS) -o $@

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build $(OBJ)

clean-pgo:
	rm -rf $(PGO_DIR)

pgo-gen-build:
	@mkdir -p $(PGO_DIR)
	$(MAKE) clean
	$(MAKE) PGO_MODE=gen

pgo-train: pgo-gen-build
	@printf 'uci\nsetoption name Workers value 1\nposition startpos\ngo movetime 1200\nposition startpos moves e2e4 c7c5 g1f3 d7d6 d2d4 c5d4 f3d4 g8f6 b1c3 a7a6\ngo movetime 1200\nposition startpos moves d2d4 d7d5 c2c4 e7e6 b1c3 g8f6 c1g5 f8e7 e2e3 e8g8\ngo movetime 1200\nquit\n' | $(BIN) > /dev/null

pgo-use-build:
	$(MAKE) clean
	$(MAKE) PGO_MODE=use

pgo: pgo-train pgo-use-build

pgo-gen: pgo-gen-build

pgo-use: pgo-use-build
