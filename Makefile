.PHONY: configure build test check fmt fmt-check tidy bench asan clean

configure:
	cmake --preset dev

build:
	cmake --build --preset dev

test:
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
