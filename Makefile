# Control Center Advanced - host build (tests, simulator) and Windows cross build (mingw-w64).
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
OEM      := $(wildcard src/oem/*.cpp)
TESTS    := $(wildcard tests/*.cpp)

OBJ_COMMON := $(patsubst src/%.cpp,$(BUILD)/%.o,$(COMMON) $(MGPCORE) $(SIM) $(OEM))
OBJ_TESTS  := $(patsubst tests/%.cpp,$(BUILD)/tests/%.o,$(TESTS))

.PHONY: all test clean i18n
all: $(BUILD)/tests.bin

$(BUILD)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(STD) $(WARN) $(OPT) $(SAN) $(INC) -MMD -MP -c $< -o $@

$(BUILD)/tests/%.o: tests/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(STD) $(WARN) $(OPT) $(SAN) $(INC) -MMD -MP -c $< -o $@

$(BUILD)/tests.bin: $(OBJ_COMMON) $(OBJ_TESTS)
	$(CXX) $(SAN) $^ -o $@ -pthread

# Every user-visible string has a Turkish translation, and no translation is left without a string (see tools/check_i18n.py).
i18n:
	@if command -v python3 >/dev/null 2>&1; then python3 tools/check_i18n.py --strict | head -1; python3 tools/check_i18n.py --strict >/dev/null; else echo "python3 not found: translation check skipped"; fi

test: i18n $(BUILD)/tests.bin
	./$(BUILD)/tests.bin

clean:
	rm -rf build

-include $(OBJ_COMMON:.o=.d) $(OBJ_TESTS:.o=.d)
