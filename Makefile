CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -D_DEFAULT_SOURCE
SRC      = src/gtpu.c src/routes.c

all: gtpu-fwd loadgen

gtpu-fwd: $(SRC) src/main.c src/gtpu.h src/routes.h
	$(CC) $(CFLAGS) -o $@ $(SRC) src/main.c

loadgen: tools/loadgen.c
	$(CC) $(CFLAGS) -pthread -o $@ tools/loadgen.c

tests/unit: $(SRC) tests/test_gtpu.c
	$(CC) $(CFLAGS) -o $@ $(SRC) tests/test_gtpu.c

test-unit: tests/unit
	./tests/unit

test-integration: gtpu-fwd
	python3 -m pytest -q tests/test_forwarder.py

test: test-unit test-integration

# AddressSanitizer + UBSan build of the unit tests.
asan:
	$(CC) -std=c11 -g -O1 -Wall -Wextra -Werror -D_DEFAULT_SOURCE \
	      -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -o tests/unit_asan $(SRC) tests/test_gtpu.c
	./tests/unit_asan

# Memory-error and leak check with Valgrind.
valgrind: tests/unit
	valgrind --error-exitcode=1 --leak-check=full --errors-for-leak-kinds=all ./tests/unit

# Throughput and latency measurement on loopback (C load generator).
# Usage: make bench RATE=0   (0 = as fast as possible) or RATE=20000
RATE ?= 0
bench: gtpu-fwd loadgen
	./gtpu-fwd -r tests/bench_routes.conf -b 127.0.0.1 -p 2152 -s 0 > .fwd.out & echo $$! > .fwd.pid; \
	sleep 0.3; \
	./loadgen -f 127.0.0.1 -p 2152 -l 9000 -t 1000 -e 2000 -n 200000 -s 128 -r $(RATE); \
	kill -TERM `cat .fwd.pid`; wait `cat .fwd.pid` 2>/dev/null; cat .fwd.out; rm -f .fwd.pid .fwd.out

clean:
	rm -f gtpu-fwd loadgen tests/unit tests/unit_asan .fwd.pid

.PHONY: all test test-unit test-integration asan valgrind bench clean
