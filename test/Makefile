# SPDX-License-Identifier: MIT
CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -Werror -O2
LIBSRC = ../src/amis_aes128.cpp ../src/amis_reader.cpp ../src/amis_iec.cpp
SRC = $(LIBSRC) test_main.cpp

test: amis_test amis_test_nostats
	./amis_test
	./amis_test_nostats

amis_test: $(SRC) ../src/*.h vectors.h
	$(CXX) $(CXXFLAGS) -I../src $(SRC) -o $@

amis_test_nostats: $(LIBSRC) test_nostats.cpp ../src/*.h vectors.h
	$(CXX) $(CXXFLAGS) -DLIBAMIS_NO_STATS -I../src $(LIBSRC) test_nostats.cpp -o $@

vectors.h: gen_vectors.py
	python3 gen_vectors.py

sanitize:
	$(CXX) -std=c++17 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I../src $(SRC) -o amis_test_san && ./amis_test_san
	$(CXX) -std=c++17 -g -fsanitize=address,undefined -fno-sanitize-recover=all -DLIBAMIS_MAX_FRAME=101 -I../src $(SRC) -o amis_test_san_small && ./amis_test_san_small

# Fuzzing with libFuzzer, ASan and UBSan (needs clang). FUZZ_TIME = seconds per target.
FUZZ_TIME ?= 60
FUZZFLAGS = -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -I../src
fuzz:
	clang++ $(FUZZFLAGS) $(LIBSRC) fuzz/fuzz_reader.cpp -o fuzz_reader
	clang++ $(FUZZFLAGS) -DLIBAMIS_MAX_FRAME=101 $(LIBSRC) fuzz/fuzz_reader.cpp -o fuzz_reader_small
	clang++ $(FUZZFLAGS) $(LIBSRC) fuzz/fuzz_iec.cpp -o fuzz_iec
	mkdir -p fuzz_corpus/reader fuzz_corpus/reader_small fuzz_corpus/iec
	./fuzz_reader -max_total_time=$(FUZZ_TIME) -max_len=2048 fuzz_corpus/reader fuzz/seeds/reader
	./fuzz_reader_small -max_total_time=$(FUZZ_TIME) -max_len=2048 fuzz_corpus/reader_small fuzz/seeds/reader
	./fuzz_iec -max_total_time=$(FUZZ_TIME) -max_len=4096 fuzz_corpus/iec fuzz/seeds/iec

clean:
	rm -f amis_test amis_test_nostats amis_test_san amis_test_san_small fuzz_reader fuzz_reader_small fuzz_iec
	rm -rf fuzz_corpus

.PHONY: test sanitize fuzz clean
