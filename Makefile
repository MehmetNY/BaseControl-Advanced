# BaseControl Advanced - host build (tests, simulator) and Windows cross build (mingw-w64).
CXX      ?= g++
STD      := -std=c++17
WARN     := -Wall -Wextra -Wpedantic -Wshadow
INC      := -Isrc
OPT      ?= -O1 -g
SAN      ?= -fsanitize=address,undefined -fno-omit-frame-pointer
BUILD    := build/host

COMMON   := $(wildcard src/common/*.cpp)
MGPCORE  := $(wildcard src/mgp/core/*.cpp)
SIM      := $(wildcard src/sim/*.cpp)
SVCPORT  := src/svc/protocol.cpp src/svc/backend.cpp
TESTS    := $(wildcard tests/*.cpp)

OBJ_COMMON := $(patsubst src/%.cpp,$(BUILD)/%.o,$(COMMON) $(MGPCORE) $(SIM) $(SVCPORT))
OBJ_TESTS  := $(patsubst tests/%.cpp,$(BUILD)/tests/%.o,$(TESTS))

.PHONY: all test clean
all: $(BUILD)/tests.bin

$(BUILD)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(STD) $(WARN) $(OPT) $(SAN) $(INC) -MMD -MP -c $< -o $@

$(BUILD)/tests/%.o: tests/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(STD) $(WARN) $(OPT) $(SAN) $(INC) -MMD -MP -c $< -o $@

$(BUILD)/tests.bin: $(OBJ_COMMON) $(OBJ_TESTS)
	$(CXX) $(SAN) $^ -o $@ -pthread

test: $(BUILD)/tests.bin
	./$(BUILD)/tests.bin

clean:
	rm -rf build

-include $(OBJ_COMMON:.o=.d) $(OBJ_TESTS:.o=.d)
