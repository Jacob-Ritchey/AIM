CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lz -lm

WIN_CC  = x86_64-w64-mingw32-gcc
WIN_BIN = aim3.exe

SRCS = main.c \
       aim3_encode.c \
       aim3_decode.c \
       bit_utils.c \
       gamma_rle.c \
       elias_fano.c \
       flags.c \
       ans.c \
       recursive.c \
       bench.c \
       buf.c \
       sha256.c

OBJS = $(SRCS:.c=.o)
BIN  = aim3

.PHONY: all clean test

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Quick round-trip smoke test using /etc/passwd (always present, short, ASCII).
test: $(BIN)
	@echo "=== Smoke test: encode + decode + diff ==="
	./$(BIN) encode /etc/passwd /tmp/aim3_test.aim3 --verbose
	./$(BIN) decode /tmp/aim3_test.aim3 /tmp/aim3_recovered.bin
	@diff /etc/passwd /tmp/aim3_recovered.bin && echo "PASS: files match" || echo "FAIL: mismatch"
	@rm -f /tmp/aim3_test.aim3 /tmp/aim3_recovered.bin

windows: $(SRCS)
	$(WIN_CC) -O2 -Wall -Wextra -std=c11 \
	    -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE \
	    -o $(WIN_BIN) $(SRCS) \
	    -static -lz -lm

clean:
	rm -f $(OBJS) $(BIN) $(WIN_BIN)
