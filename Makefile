.PHONY: configure build test check fmt fmt-check tidy bench bench-diff bench-allocator bench-storage perf-stat perf-record numa-study false-sharing-study profile-io socket-stress socket-load concurrency-stress asan tsan demo check-fixtures check-manifest determinism divergence-demo clean

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

bench-storage:
	cmake --preset bench
	cmake --build --preset bench --target qsl-bench
	QSL_BENCH_BIN=build/bench/qsl-bench bash scripts/run_storage_benchmarks.sh

perf-stat:
	@test "$$(uname -s)" = "Linux" || { echo "error: make perf-stat requires Linux perf; current OS is $$(uname -s)." >&2; exit 2; }
	cmake --preset bench
	cmake --build --preset bench --target qsl-bench
	QSL_BENCH_BIN=build/bench/qsl-bench bash scripts/perf_stat.sh

perf-record:
	@test "$$(uname -s)" = "Linux" || { echo "error: make perf-record requires Linux perf; current OS is $$(uname -s)." >&2; exit 2; }
	cmake --preset bench
	cmake --build --preset bench --target qsl-bench
	QSL_BENCH_BIN=build/bench/qsl-bench bash scripts/perf_record.sh

# M43: CPU-affinity / scheduler-migration / NUMA locality study. Linux-only.
numa-study:
	@if test "$$(uname -s)" != "Linux"; then \
		bash scripts/numa_affinity_study.sh; \
		exit $$?; \
	fi
	cmake --preset bench
	cmake --build --preset bench --target qsl-bench
	QSL_NUMA_BIN=build/bench/qsl-bench bash scripts/numa_affinity_study.sh

# M44: benchmark-only packed-vs-padded SPSC cursor false-sharing study.
false-sharing-study:
	cmake --preset bench
	cmake --build --preset bench --target qsl-bench
	QSL_FALSE_SHARING_BIN=build/bench/qsl-bench bash scripts/run_false_sharing_study.sh

# M30: syscall / kernel-socket path profile of the gateway (strace + procfs rusage). Linux-only.
profile-io:
	@test "$$(uname -s)" = "Linux" || { echo "error: make profile-io requires Linux (strace + procfs); current OS is $$(uname -s)." >&2; exit 2; }
	cmake --preset dev
	cmake --build --preset dev --target qsl-gateway qsl-client
	bash scripts/profile_gateway_io.sh

# M30: UDP burst/gap + receive-socket-buffer experiment over loopback. Portable (Linux/macOS).
socket-stress: build
	bash scripts/socket_stress.sh

# M35: multi-client TCP connection-scaling load (blocking vs epoll gateway). Linux-only.
socket-load:
	@test "$$(uname -s)" = "Linux" || { echo "error: make socket-load requires Linux (epoll + hi-res timer); current OS is $$(uname -s)." >&2; exit 2; }
	cmake --preset dev
	cmake --build --preset dev --target qsl-gateway qsl-client
	bash scripts/socket_load.sh

concurrency-stress:
	bash scripts/concurrency_stress.sh

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
