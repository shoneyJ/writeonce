CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?= -pthread

wo-rt: wo-rt.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# ---- wovm VM core (src/) + unit tests (test/) ----
# Each test/test_*.c builds into its own ASan+UBSan binary linked against
# every src/*.c except main.c; `make test` runs them all.
# the database engine lives in its own top-level directory (iteration 9;
# user decision 2026-08-15) and is statically linked into every wovm and
# every test binary — one binary, unchanged
DBSRC   := $(wildcard ../database/src/*.c)
DBHDR   := $(wildcard ../database/src/*.h)
VMSRC   := $(filter-out src/main.c,$(wildcard src/*.c)) $(DBSRC)
VMHDR   := $(wildcard src/*.h) $(wildcard test/*.h) $(DBHDR)
TESTS   := $(wildcard test/test_*.c)
TESTBIN := $(TESTS:test/%.c=build/%)
TCFLAGS := -std=c11 -Wall -Wextra -Werror -g -O1 \
           -fsanitize=address,undefined -fno-omit-frame-pointer

build:
	mkdir -p build

TESTHELP := $(wildcard test/wob_build.c)

build/%: test/%.c $(VMSRC) $(TESTHELP) $(VMHDR) | build
	$(CC) $(TCFLAGS) -Isrc -Itest -I../database/src -o $@ $< $(TESTHELP) $(VMSRC)

test: $(TESTBIN)
	@for t in $(TESTBIN); do echo "== $$t"; ./$$t || exit 1; done

# ISO dispatch flavor (switch instead of computed goto) — gated in every
# run so the fallback can never rot
ISOBIN := $(TESTS:test/%.c=build/iso_%)

build/iso_%: test/%.c $(VMSRC) $(TESTHELP) $(VMHDR) | build
	$(CC) $(TCFLAGS) -DWO_ISO_C -Isrc -Itest -I../database/src -o $@ $< $(TESTHELP) $(VMSRC)

test-iso: $(ISOBIN)
	@for t in $(ISOBIN); do echo "== $$t"; ./$$t || exit 1; done

# the wovm binary (plain optimized build; the test suite is the ASan gate)
wovm: src/main.c $(VMSRC) $(VMHDR)
	$(CC) $(CFLAGS) -Isrc -I../database/src -o $@ src/main.c $(VMSRC)

# ASan+UBSan wovm, same flags as the unit tests, for corpus fixtures that
# need a sanitizer to prove a free actually happened (gc/ cycle fixtures) —
# tasks 3/4 hand-built this each time because it didn't exist yet
build/wovm_asan: src/main.c $(VMSRC) $(VMHDR) | build
	$(CC) $(TCFLAGS) -Isrc -I../database/src -o $@ src/main.c $(VMSRC)

wovm-asan: build/wovm_asan

# fixture generator for the CLI smoke test
build/mkwob: test/mkwob.c test/wob_build.c $(VMHDR) | build
	$(CC) $(CFLAGS) -Isrc -Itest -o $@ test/mkwob.c test/wob_build.c

bench/bench: bench/bench.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

bench: bench/bench

run: wo-rt
	./wo-rt

clean:
	rm -f wo-rt bench/bench wovm
	rm -rf build

.PHONY: bench run clean test test-iso wovm-asan
