CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?= -lm

.PHONY: all run clean sample

all: donut object

donut: donut.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

object: object.c stb_image.h
	$(CC) $(CFLAGS) -o $@ object.c $(LDFLAGS)

sample.png: make_sample.py
	python3 make_sample.py

run: donut
	./donut

run-object: object
	./object Kir-Dev-White.png

clean:
	rm -f donut object
