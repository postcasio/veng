#!/usr/bin/env sh
# Measure how much of veng's own sources the test suite exercises.
#
#   scripts/coverage.sh
#   COVERAGE_CTEST_ARGS="-L unit" scripts/coverage.sh
#   COVERAGE_CTEST_ARGS="" scripts/coverage.sh          # every test, CMake-script ones included
#
# Configures a dedicated build-coverage/ with VENG_ENABLE_COVERAGE=ON, builds it,
# runs a CTest selection, and writes a gcovr report:
#
#   build-coverage/coverage.json            machine-readable, line- and branch-level
#   build-coverage/coverage-html/index.html the same report for direct viewing
#
# Coverage gets its own build tree because the gcov flags (--coverage, -O0) and the
# suppressed PCH are at odds with the primary build's caching, and because the .gcda
# counters a run leaves behind should not accumulate in a tree used for ordinary work.
#
# The selection is one variable: COVERAGE_CTEST_ARGS defaults to every test that runs a
# compiled binary, and takes any CTest argument list to change that. The default excludes
# the CMake-script tests (add_test running `cmake -P`), for two distinct reasons:
#
#   - They produce no counters for this tree. sdk_conformance_* configures and builds a
#     separate, uninstrumented SDK tree; cook_per_config reconfigures and rebuilds;
#     validation_gate re-runs gpu binaries the selection already ran.
#   - smoke_golden does run instrumented code, through the launcher, but asserts only that
#     a rendered capture matches a golden image. The lines it marks covered are not thereby
#     shown correct, so counting them inflates the report without evidencing anything.
#
# Setting the variable replaces the default outright, so an explicit value can put them back.
#
# gcov merges the per-process counters, so changing the selection needs no other change.
# Narrow with one -L and a regex alternation: CTest requires a test to match every -L
# given, so repeating the flag intersects the labels and usually selects nothing.
#
# Prerequisites are gcovr and a gcov-compatible tool. gcovr drives either GCC's gcov
# or Clang's llvm-cov gcov via --gcov-executable, which is what makes the same report
# reproducible on both toolchains; on macOS the backend is `xcrun llvm-cov gcov`.
set -eu

cd "$(dirname "$0")/.."

BUILD_DIR=build-coverage
COVERAGE_CTEST_ARGS="${COVERAGE_CTEST_ARGS:--E (sdk_conformance|validation_gate|cook_per_config|smoke_golden)}"

# Instrumented objects cache correctly — ccache restores the .gcno note beside the object —
# but they are many times the size of the primary build's and that build never reads them
# back. Sharing one cache therefore spends the primary build's entries on objects only
# coverage wants. Scope the cache to this tree instead: coverage still gets full reuse across
# its own runs, bounded by its own ceiling, in a store that dies with the tree.
export CCACHE_DIR="${CCACHE_DIR:-$PWD/$BUILD_DIR/.ccache}"
export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-5G}"

if [ "$(uname)" = Darwin ]; then
    GCOV="${GCOV:-xcrun llvm-cov gcov}"
else
    GCOV="${GCOV:-gcov}"
fi

if ! command -v gcovr >/dev/null 2>&1; then
    echo "coverage: gcovr not found on PATH." >&2
    echo "          Install it with 'brew install gcovr' or 'pip install gcovr'." >&2
    exit 1
fi

if ! $GCOV --version >/dev/null 2>&1; then
    echo "coverage: the gcov backend '$GCOV' does not run." >&2
    echo "          gcovr needs GCC's gcov or Clang's llvm-cov gcov; install one and" >&2
    echo "          re-run, or set GCOV to a working gcov-compatible tool." >&2
    exit 1
fi

cmake -B "$BUILD_DIR" -S . -G Ninja -DVE_DEBUG=ON -DVENG_ENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build "$BUILD_DIR"

# Counters from an earlier run would be merged into this one's totals.
find "$BUILD_DIR" -name '*.gcda' -delete

# The selection reports its own failures; the report is still emitted so a partial
# run is inspectable, and the script exits non-zero at the end.
ctest_status=0
# shellcheck disable=SC2086
ctest --test-dir "$BUILD_DIR" --output-on-failure $COVERAGE_CTEST_ARGS || ctest_status=$?

mkdir -p "$BUILD_DIR/coverage-html"
# A template or inline function instantiated in several TUs is attributed to different
# lines in each one's gcov output, which gcovr's default strict function merge rejects
# outright. Reconciling those records onto the lowest line keeps the report whole; line
# coverage, which is what the report is read for, is identical either way.
gcovr --root . \
      --gcov-executable "$GCOV" \
      --merge-mode-functions merge-use-line-min \
      --exclude "$BUILD_DIR/_deps/" \
      --exclude 'tests/' \
      --exclude-unreachable-branches \
      --exclude-throw-branches \
      --json "$BUILD_DIR/coverage.json" \
      --html-details "$BUILD_DIR/coverage-html/index.html" \
      --print-summary

echo "coverage: wrote $BUILD_DIR/coverage.json and $BUILD_DIR/coverage-html/index.html"

exit $ctest_status
