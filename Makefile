# Matching engine build.
#
# Targets:
#   make            build the engine          -> build/matching_engine
#   make test       build + run unit tests
#   make datasets   build + diff data/*.in against expected output
#   make bench      build + run a throughput benchmark (make bench COUNT=2000000)
#   make all-checks build, then run tests and dataset checks
#   make clean      remove build artifacts

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
LDFLAGS  ?=

SRC_DIR   := src
TEST_DIR  := tests
BUILD_DIR := build

# Library sources shared by the engine and the tests.
LIB_SRCS  := $(SRC_DIR)/OrderBook.cpp $(SRC_DIR)/MessageParser.cpp

ENGINE_SRCS := $(SRC_DIR)/main.cpp $(LIB_SRCS)
ENGINE_BIN  := $(BUILD_DIR)/matching_engine

TEST_SRCS := $(TEST_DIR)/test_main.cpp $(LIB_SRCS)
TEST_BIN  := $(BUILD_DIR)/run_tests

COUNT ?= 1000000

.PHONY: all test datasets bench all-checks clean

all: $(ENGINE_BIN)

$(ENGINE_BIN): $(ENGINE_SRCS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(ENGINE_SRCS) -o $@ $(LDFLAGS)

$(TEST_BIN): $(TEST_SRCS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(TEST_SRCS) -o $@ $(LDFLAGS)

test: $(TEST_BIN)
	./$(TEST_BIN)

datasets: $(ENGINE_BIN)
	bash scripts/run_datasets.sh ./$(ENGINE_BIN)

bench: $(ENGINE_BIN)
	bash scripts/bench.sh ./$(ENGINE_BIN) $(COUNT)

all-checks: test datasets

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
