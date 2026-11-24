# SPDX-License-Identifier: MIT
CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -Werror -O2
SRC = ../src/amis_aes128.cpp ../src/amis_reader.cpp ../src/amis_iec.cpp test_main.cpp

test: amis_test
	./amis_test

amis_test: $(SRC) ../src/*.h vectors.h
	$(CXX) $(CXXFLAGS) -I../src $(SRC) -o $@

vectors.h: gen_vectors.py
	python3 gen_vectors.py

sanitize:
	$(CXX) -std=c++17 -g -fsanitize=address,undefined -I../src $(SRC) -o amis_test_san && ./amis_test_san
	$(CXX) -std=c++17 -g -fsanitize=address,undefined -DLIBAMIS_MAX_FRAME=101 -I../src $(SRC) -o amis_test_san_small && ./amis_test_san_small

clean:
	rm -f amis_test amis_test_san amis_test_san_small

.PHONY: test sanitize clean
