CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?= -lm

.PHONY: all run clean sample

all: Kir-Console

Kir-Console: Kir-Console.c stb_image.h
	$(CC) $(CFLAGS) -o $@ Kir-Console.c $(LDFLAGS)

sample.png: make_sample.py
	python3 make_sample.py

run-Kir-Console: Kir-Console
	./Kir-Console Kir-Dev-White.png

clean:
	rm -f Kir-Console
