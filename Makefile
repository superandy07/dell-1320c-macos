# Dell Color Laser 1320c native driver for macOS

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-deprecated-declarations
ARCHS   ?= -arch arm64 -arch x86_64
MINOS   ?= -mmacosx-version-min=11.0

all: build/rastertodell1320c build/hbpl1x

build/rastertodell1320c: src/rastertodell1320c.c src/lut_data.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(ARCHS) $(MINOS) -o $@ src/rastertodell1320c.c -lcups

# decoder used by the tests to check what the filter sends
build/hbpl1x: tools/hbpl1x.c
	@mkdir -p build
	$(CC) -O2 -Wall -o $@ tools/hbpl1x.c

src/lut_data.h: tools/genlut.py
	@test -f $@ || { echo "src/lut_data.h missing: run tools/genlut.py on the measured tables"; exit 1; }

test: all
	./test/run_tests.sh

clean:
	rm -rf build test/out

.PHONY: all test clean
