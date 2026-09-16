CC ?= gcc
CFLAGS ?= -O3 -march=native -std=c11 -Wall -Wextra -Isrc
LDFLAGS ?= -lz -lm

.PHONY: all test clean bench-xml

BIN := bin/npcc

all: $(BIN)

$(BIN): src/main.o src/codec.o src/tnssrc.o src/riser.o src/bwt.o src/parse_rep4.o
	mkdir -p bin
	$(CC) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c src/npcc.h src/tnssrc.h src/riser.h src/bwt.h src/parse_rep4.h
	$(CC) $(CFLAGS) -c -o $@ $<

tests/test_npcc: tests/test_npcc.c src/codec.o src/tnssrc.o src/bwt.o src/parse_rep4.o src/npcc.h
	$(CC) $(CFLAGS) -o $@ tests/test_npcc.c src/codec.o src/tnssrc.o src/bwt.o src/parse_rep4.o $(LDFLAGS)

tests/test_riser: tests/test_riser.c src/riser.o src/riser.h
	$(CC) $(CFLAGS) -o $@ tests/test_riser.c src/riser.o $(LDFLAGS)

test: tests/test_npcc tests/test_riser
	./tests/test_npcc
	./tests/test_riser

bench-xml: $(BIN)
	$(BIN) bench /home/ceedotrock/data/silesia/xml classical

clean:
	rm -f bin/npcc src/*.o tests/test_npcc tests/test_riser
