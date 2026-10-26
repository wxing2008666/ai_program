CXX ?= g++
CXXFLAGS ?= -std=c++11 -O2 -g -Wall -Wextra -Wpedantic -Wshadow -pthread
CPPFLAGS ?= -Iinclude
LDFLAGS ?=
LDLIBS ?= -pthread

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj

CORE_SRC := $(filter-out src/main.cpp,$(wildcard src/*.cpp))
DEMO_SRC := src/main.cpp
TEST_SRC := $(wildcard tests/*.cpp)

CORE_OBJ := $(patsubst src/%.cpp,$(OBJ_DIR)/%.o,$(CORE_SRC))
DEMO_OBJ := $(patsubst src/%.cpp,$(OBJ_DIR)/%.o,$(DEMO_SRC))
TEST_OBJ := $(patsubst tests/%.cpp,$(OBJ_DIR)/tests/%.o,$(TEST_SRC))
DEPS := $(CORE_OBJ:.o=.d) $(DEMO_OBJ:.o=.d) $(TEST_OBJ:.o=.d)

.PHONY: all demo tests run test clean

all: demo tests

demo: $(BUILD_DIR)/spl_demo
tests: $(BUILD_DIR)/spl_tests

$(BUILD_DIR)/spl_demo: $(CORE_OBJ) $(DEMO_OBJ) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/spl_tests: $(CORE_OBJ) $(TEST_OBJ) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(OBJ_DIR)/%.o: src/%.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR)/tests/%.o: tests/%.cpp | $(OBJ_DIR)/tests
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR) $(OBJ_DIR) $(OBJ_DIR)/tests:
	mkdir -p $@

run: demo
	./$(BUILD_DIR)/spl_demo

test: tests
	./$(BUILD_DIR)/spl_tests

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
