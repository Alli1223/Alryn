# Convenience wrapper around the CMake build (the real build system).
# CMake generates its own makefiles inside $(BUILD_DIR); this top-level Makefile
# just gives you short commands: `make`, `make run`, `make test`, `make clean`.
#
#   make            configure + build everything
#   make run        build, then launch the game
#   make test       build, then run the test suite
#   make clean      delete the build directory
#   make rebuild    clean + build
#
# Override defaults like:  make BUILD_TYPE=Release run   (run `make clean` first
# when switching BUILD_TYPE, since the choice is cached by CMake).

BUILD_DIR  ?= build
BUILD_TYPE ?= Debug
JOBS       ?= $(shell nproc 2>/dev/null || echo 4)
CMAKE      ?= cmake

GAME  := $(BUILD_DIR)/bin/alryn_game
CACHE := $(BUILD_DIR)/CMakeCache.txt

.PHONY: all build configure run test clean rebuild help
.DEFAULT_GOAL := build

all: build

## configure: generate the build system (idempotent)
configure: $(CACHE)

$(CACHE):
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

## build: compile the engine, game, and tests
build: configure
	@MAKEFLAGS= $(CMAKE) --build $(BUILD_DIR) -j$(JOBS)

## run: build, then launch the game
run: build
	@echo "==> $(GAME)"
	@./$(GAME)

## test: build, then run the test suite
test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

## clean: remove the build directory
clean:
	rm -rf $(BUILD_DIR)

## rebuild: clean, then build from scratch
rebuild: clean build

## help: list available targets
help:
	@grep -E '^## ' $(MAKEFILE_LIST) | sed -e 's/## //'
