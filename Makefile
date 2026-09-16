CC ?= gcc
CFLAGS ?= -O3 -march=native -std=c11 -Wall -Wextra -Isrc
LDFLAGS ?= -lz -lm

.PHONY: all test clean bench-xml

BIN := bin/npcc

all: $(BIN)

$(BIN): src/main.o src/codec.o src/tnssrc.o src/riser.o src/bwt.o src/parse_rep4.o src/lzm2.o src/frontend.o src/workbench.o
	mkdir -p bin
	$(CC) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c src/npcc.h src/tnssrc.h src/riser.h src/bwt.h src/parse_rep4.h src/lzm2.h src/frontend.h src/workbench.h
	$(CC) $(CFLAGS) -c -o $@ $<

tests/test_npcc: tests/test_npcc.c src/codec.o src/tnssrc.o src/bwt.o src/parse_rep4.o src/lzm2.o src/frontend.o src/npcc.h
	$(CC) $(CFLAGS) -o $@ tests/test_npcc.c src/codec.o src/tnssrc.o src/bwt.o src/parse_rep4.o src/lzm2.o src/frontend.o $(LDFLAGS)

tests/test_lzm2: tests/test_lzm2.c src/lzm2.o src/lzm2.h
	$(CC) $(CFLAGS) -o $@ tests/test_lzm2.c src/lzm2.o $(LDFLAGS)

tests/test_riser: tests/test_riser.c src/riser.o src/riser.h
	$(CC) $(CFLAGS) -o $@ tests/test_riser.c src/riser.o $(LDFLAGS)

tests/test_frontend: tests/test_frontend.c src/frontend.o src/frontend.h
	$(CC) $(CFLAGS) -o $@ tests/test_frontend.c src/frontend.o $(LDFLAGS)

tests/test_workbench: tests/test_workbench.c src/workbench.o src/tnssrc.o src/codec.o src/bwt.o src/parse_rep4.o src/lzm2.o src/frontend.o src/workbench.h src/tnssrc.h
	$(CC) $(CFLAGS) -o $@ tests/test_workbench.c src/workbench.o src/tnssrc.o src/codec.o src/bwt.o src/parse_rep4.o src/lzm2.o src/frontend.o $(LDFLAGS)

test: tests/test_npcc tests/test_riser tests/test_lzm2 tests/test_frontend tests/test_workbench
	./tests/test_npcc
	./tests/test_riser
	./tests/test_lzm2
	./tests/test_frontend
	./tests/test_workbench

bench-xml: $(BIN)
	$(BIN) bench /home/ceedotrock/data/silesia/xml classical

clean:
	rm -f bin/npcc src/*.o tests/test_npcc tests/test_riser
