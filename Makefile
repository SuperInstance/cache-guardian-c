CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -Iinclude
LDFLAGS = -lm

SRC     = src/cache_guardian.c
OBJ     = $(SRC:.c=.o)

.PHONY: all test clean

all: libcacheguardian.a

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

libcacheguardian.a: $(OBJ)
	ar rcs $@ $^

test: tests/test_cache_guardian.c libcacheguardian.a
	$(CC) $(CFLAGS) $< -L. -lcacheguardian $(LDFLAGS) -o test_runner
	./test_runner

clean:
	rm -f src/*.o libcacheguardian.a test_runner
