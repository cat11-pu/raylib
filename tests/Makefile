CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -I../src
LDLIBS = ../src/libraylib.a -lm

.PHONY: all asan clean lib

all: test_lod

lib:
	$(MAKE) -C ../src PLATFORM=PLATFORM_MEMORY RAYLIB_LIBTYPE=STATIC

test_lod: test_lod.c lib
	$(CC) $(CFLAGS) test_lod.c -o $@ $(LDLIBS)

asan: test_lod.c lib
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined -fno-omit-frame-pointer test_lod.c -o test_lod_asan $(LDLIBS)

clean:
	rm -f test_lod test_lod_asan
