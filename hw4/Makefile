CC     = gcc
CFLAGS = -Wall -Wextra -O0 -g

all: main multilevelBF.so

main: main.c
	$(CC) $(CFLAGS) $< -o $@

multilevelBF.so: multilevelBF.c
	$(CC) $(CFLAGS) -shared -fPIC $< -o $@

# Usage: make run T=test1.txt
T ?= test1.txt
run: all
	cp $(T) test.txt
	LD_PRELOAD=$(CURDIR)/multilevelBF.so ./main

clean:
	rm -f main multilevelBF.so test.txt

.PHONY: all run clean
