# Convenience wrapper around the CMake build (the real build system).
# CMake generates its own makefiles inside $(BUILD_DIR); this top-level Makefile
# just gives you short commands.
#
#   make                 configure + build everything
#   make run             build, then launch the game
#   make test            build, then run the whole test suite
#   make clean           delete the build directory
#   make rebuild         clean + build
#   make ci              clean build + full test run (mirrors GitHub CI locally)
#
# Per-subsystem tests (fast TDD loops - build once, run just one area):
#   make test-core       Core (types, math, events, ...)
#   make test-scene      Scene graph / components / camera
#   make test-physics    Physics + collisions (capsule push-out, house walls, ...)
#   make test-render     Renderer (offscreen render, device, buffers)
#   make test-terrain    Terrain meshing + world scatter (trees/props/houses)
#   make test-net        Networking (protocol, server, integration)
#   make test-combat     Enemies + combat (melee cone, enemy AI, damage)
#   make test-character  Character model + animation
#   make test-ui         UI widgets / vector font
#   make test-filter FILTER="*House*"   run any subset by doctest test-case glob
#   make list-tests      list every test case
#   make shots           (re)generate offscreen render screenshots (PPM/PNG)
#
# Override defaults like:  make BUILD_TYPE=Release run   (run `make clean` first
# when switching BUILD_TYPE, since the choice is cached by CMake).

BUILD_DIR  ?= build
BUILD_TYPE ?= Debug
JOBS       ?= $(shell nproc 2>/dev/null || echo 4)
CMAKE      ?= cmake
FILTER     ?= *

GAME    := $(BUILD_DIR)/bin/alryn_game
TESTBIN := $(BUILD_DIR)/bin/alryn_tests
CACHE   := $(BUILD_DIR)/CMakeCache.txt

.PHONY: all build configure run test clean rebuild ci help \
        test-core test-scene test-physics test-render test-terrain \
        test-net test-combat test-character test-ui test-filter list-tests shots

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

## test: build, then run the whole test suite
test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

## ci: clean configure + build + test, the way GitHub CI does (catch everything)
ci:
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)
	$(CMAKE) --build $(BUILD_DIR) -j$(JOBS)
	ctest --test-dir $(BUILD_DIR) --output-on-failure

# --- Per-subsystem test runners (filter the single doctest binary by source file)
# Build the test target once, then run only the matching file's tests.
test-core:      build ; @$(TESTBIN) "--source-file=*core_tests*"
test-scene:     build ; @$(TESTBIN) "--source-file=*scene_tests*"
test-physics:   build ; @$(TESTBIN) "--source-file=*physics_tests*"
test-render:    build ; @$(TESTBIN) "--source-file=*render_offscreen_tests*,*renderer_tests*,*mesh_tests*,*scene_shot_tests*"
test-terrain:   build ; @$(TESTBIN) "--source-file=*terrain_tests*"
test-net:       build ; @$(TESTBIN) "--source-file=*net_tests*"
test-combat:    build ; @$(TESTBIN) "--source-file=*combat_tests*"
test-character: build ; @$(TESTBIN) "--source-file=*character_tests*"
test-ui:        build ; @$(TESTBIN) "--source-file=*ui_tests*"

## test-filter: run a subset by test-case name glob, e.g. make test-filter FILTER="*collision*"
test-filter: build
	@$(TESTBIN) "--test-case=$(FILTER)"

## list-tests: list every registered test case
list-tests: build
	@$(TESTBIN) --list-test-cases

## shots: run the offscreen render tests; they write *.ppm next to the test binary
shots: build
	@$(TESTBIN) "--source-file=*render_offscreen_tests*,*scene_shot_tests*" --success
	@if command -v magick >/dev/null 2>&1; then \
	  for f in $(BUILD_DIR)/bin/*.ppm; do [ -e "$$f" ] && magick "$$f" "$${f%.ppm}.png"; done; \
	  echo "Converted PPM -> PNG in $(BUILD_DIR)/bin/"; \
	fi
	@echo "Screenshots in $(BUILD_DIR)/bin/ (*.ppm):"; ls -1 $(BUILD_DIR)/bin/*.ppm 2>/dev/null || echo "  (none - no GPU/shaders on this machine)"

## clean: remove the build directory
clean:
	rm -rf $(BUILD_DIR)

## rebuild: clean, then build from scratch
rebuild: clean build

## help: list available targets
help:
	@grep -E '^## ' $(MAKEFILE_LIST) | sed -e 's/## //'
