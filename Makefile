CC ?= gcc

C_STD ?= -std=c11
WARNINGS ?= -Wall -Wextra -Wpedantic
NATIVE_OPT ?= -Ofast -march=native -mtune=native -flto -funroll-loops
THREAD_FLAGS ?= -pthread

CPPFLAGS ?= -D_POSIX_C_SOURCE=200809L
CFLAGS ?= $(C_STD) $(WARNINGS) $(NATIVE_OPT) $(THREAD_FLAGS)
LDFLAGS ?= $(THREAD_FLAGS)
LDLIBS ?=

SRC := $(shell find src -name '*.c' | sort)
OBJ := $(SRC:.c=.o)
BIN := build/chess_bot

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(OBJ) $(LDFLAGS) $(LDLIBS) -o $@

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build $(OBJ)
