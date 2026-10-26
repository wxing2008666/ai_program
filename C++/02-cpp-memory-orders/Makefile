CXX ?= g++
CXXFLAGS ?= -std=c++11 -O2 -g -Wall -Wextra -Wpedantic -Wshadow -pthread
CPPFLAGS ?= -Iinclude
LDLIBS ?= -pthread

BUILD_DIR := build
TEST_SRC := $(wildcard tests/*.cpp)

.PHONY: all demo tests run test clean

all: demo tests

demo: $(BUILD_DIR)/mo_demo

tests: $(BUILD_DIR)/mo_tests

$(BUILD_DIR)/mo_demo: src/main.cpp $(wildcard include/mo/*.h)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LDLIBS) -o $@

$(BUILD_DIR)/mo_tests: $(TEST_SRC) $(wildcard include/mo/*.h)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(TEST_SRC) $(LDLIBS) -o $@

run: demo
	./$(BUILD_DIR)/mo_demo

test: tests
	./$(BUILD_DIR)/mo_tests

clean:
	rm -rf $(BUILD_DIR)
