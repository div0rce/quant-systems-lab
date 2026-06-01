.PHONY: configure build test check fmt fmt-check tidy bench bench-diff bench-allocator asan tsan demo check-fixtures check-manifest determinism divergence-demo clean

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
	@find include src tests apps benchmarks -name '*.hpp' -o -name '*.cpp' | xargs clang-format -i

fmt-check:
	@find include src tests apps benchmarks -name '*.hpp' -o -name '*.cpp' | xargs clang-format --dry-run --Werror

tidy:
	@echo "clang-tidy: run manually with compile_commands.json from build/dev"
	@echo "  run-clang-tidy -p build/dev include/ src/"

bench:
	cmake --preset bench
	cmake --build --preset bench --target qsl-bench
	QSL_BENCH_BIN=build/bench/qsl-bench bash scripts/run_benchmarks.sh

bench-diff:
	cmake --preset bench
	cmake --build --preset bench --target qsl-bench
	QSL_BENCH_BIN=build/bench/qsl-bench bash scripts/run_diff_benchmarks.sh

bench-allocator:
	cmake --preset bench
	cmake --build --preset bench --target qsl-bench
	QSL_BENCH_BIN=build/bench/qsl-bench bash scripts/run_allocator_experiment.sh

asan:
	cmake --preset asan
	cmake --build --preset asan
	ctest --preset asan

# ThreadSanitizer: data-race gate for the concurrent pipeline (M27). Runs only the
# concurrency-labelled tests; never used for performance measurement.
tsan:
	cmake --preset tsan
	cmake --build --preset tsan
	ctest --preset tsan -L concurrency

demo: build
	bash scripts/demo.sh

check-fixtures: build
	bash scripts/check_fixtures.sh

check-manifest: build
	bash scripts/fixture_manifest.sh --check

determinism:
	bash scripts/determinism_check.sh

divergence-demo: build
	bash scripts/divergence_demo.sh

clean:
	rm -rf build
