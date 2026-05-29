.PHONY: configure build test check fmt fmt-check tidy bench asan clean

BUILD_DIR := build/dev

$(BUILD_DIR)/CMakeCache.txt:
	cmake --preset dev

configure: $(BUILD_DIR)/CMakeCache.txt

build: $(BUILD_DIR)/CMakeCache.txt
	cmake --build --preset dev

test: build
	ctest --preset dev

check: fmt-check build test

fmt:
	@find include src tests -name '*.hpp' -o -name '*.cpp' | xargs clang-format -i

fmt-check:
	@find include src tests -name '*.hpp' -o -name '*.cpp' | xargs clang-format --dry-run --Werror

tidy:
	@echo "clang-tidy: run manually with compile_commands.json from build/dev"
	@echo "  run-clang-tidy -p build/dev include/ src/"

bench:
	@echo "Benchmarks not yet implemented (M11)."

asan:
	cmake --preset asan
	cmake --build --preset asan
	ctest --preset asan

clean:
	rm -rf build
