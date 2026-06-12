CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?= -pthread

wo-rt: wo-rt.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

bench/bench: bench/bench.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

bench: bench/bench

run: wo-rt
	./wo-rt

clean:
	rm -f wo-rt bench/bench

.PHONY: bench run clean
