CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++20 -Wall -Wextra -pthread
PQ_CFLAGS := $(shell pkg-config --cflags libpq 2>/dev/null)
PQ_LIBS   := $(shell pkg-config --libs libpq 2>/dev/null)
ifeq ($(strip $(PQ_LIBS)),)
  PQ_LIBS := -lpq
endif

BIN  := chbench
SRC  := chbench.cpp
HDRS := chbench_base.h tpcc_benchbase.h

.PHONY: all clean

all: $(BIN)

$(BIN): $(SRC) $(HDRS)
	$(CXX) $(CXXFLAGS) $(PQ_CFLAGS) -o $@ $< $(PQ_LIBS)

clean:
	rm -f $(BIN)
