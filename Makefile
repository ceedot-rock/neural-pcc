CC ?= gcc
CFLAGS ?= -O3 -std=c11 -Wall -Wextra -Isrc
LDFLAGS ?= -lz -lm

.PHONY: all test clean bench-xml

BIN := bin/npcc

all: $(BIN)

$(BIN): src/main.o src/codec.o src/tnssrc.o src/riser.o
	mkdir -p bin
	$(CC) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c src/npcc.h src/tnssrc.h src/riser.h
	$(CC) $(CFLAGS) -c -o $@ $<

tests/test_npcc: tests/test_npcc.c src/codec.o src/tnssrc.o src/npcc.h
	$(CC) $(CFLAGS) -o $@ tests/test_npcc.c src/codec.o src/tnssrc.o $(LDFLAGS)

tests/test_riser: tests/test_riser.c src/riser.o src/riser.h
	$(CC) $(CFLAGS) -o $@ tests/test_riser.c src/riser.o $(LDFLAGS)

test: tests/test_npcc tests/test_riser
	./tests/test_npcc
	./tests/test_riser

bench-xml: $(BIN)
	$(BIN) bench /home/ceedotrock/data/silesia/xml classical

clean:
	rm -f bin/npcc src/*.o tests/test_npcc tests/test_riser
