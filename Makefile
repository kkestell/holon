CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic -O2
PYTHON ?= python3
LLVM ?= $(shell brew --prefix llvm)

all: holon

holon: holon.c
	$(CC) $(CFLAGS) $< -o $@

test:
	$(PYTHON) test.py

test-sanitize:
	$(PYTHON) test.py --sanitize

check:
	$(LLVM)/bin/clang-tidy --quiet holon.c -- -std=c11

format:
	$(LLVM)/bin/clang-format -i holon.c

clean:
	rm -f holon

.PHONY: all test test-sanitize check format clean
