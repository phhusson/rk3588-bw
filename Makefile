CC ?= cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64 -Isrc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
LDLIBS += -lm
PREFIX ?= /usr/local

.PHONY: all check check-hardware clean install
all: rk3588-bw

rk3588-bw: src/main.c src/probe.c src/topology.c src/probe.h src/topology.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ src/main.c src/probe.c src/topology.c $(LDLIBS)

build/test-core: tests/test-core.c src/topology.c src/topology.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test-core.c src/topology.c $(LDLIBS)

check: rk3588-bw build/test-core
	./build/test-core
	python3 tests/test-cli.py

build/test-filter-hardware: tests/test-filter-hardware.c src/probe.c src/topology.c src/probe.h src/topology.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test-filter-hardware.c src/probe.c src/topology.c $(LDLIBS)

# Explicitly opt in: this programs actual RK3588 probe hardware.
check-hardware: rk3588-bw build/test-filter-hardware
	python3 tests/test-hardware.py
	./build/test-filter-hardware

install: rk3588-bw
	install -Dm755 rk3588-bw $(DESTDIR)$(PREFIX)/bin/rk3588-bw

clean:
	rm -f rk3588-bw build/test-core build/test-filter-hardware
