#!/usr/bin/env bash
# Run clang-tidy over the named sources against the repo-root .clang-tidy.
#
#   scripts/tidy.sh --db build-debug engine/src/Gui/Document.cpp [more.cpp ...]
#
# Name the build tree: a checkout carries several, and the search is a fallback that returns
# the first that exists, which is not necessarily the one the work is built against.
#
# The toolchain plumbing this shares with .githooks/pre-commit — resolving and vetting the
# build tree, the PCH-stripped DB copy, the include paths libTooling needs, and telling a run
# that found nothing apart from a run that never started — lives in scripts/lib/tidy-common.sh.
#
# The sysroot half of the problem is fixed at configure time rather than here: the build tree
# is configured with CMAKE_OSX_SYSROOT so the DB records -isysroot. Without it every TU dies on
# 'cstdint' file not found, since the compiler resolves the SDK implicitly as a driver and
# libTooling does not.
#
# The sysroot must be set when the tree is FIRST configured. CMake computes the
# implicit-include list once and caches it, and on this host /usr/local/include — where the
# Vulkan and Slang headers live — is on it, so CMake strips -I for that directory. A fresh
# sysroot configure recomputes the list without it and emits -isystem instead; adding the flag
# to an existing tree leaves the stale list in place, so CMake keeps stripping while the
# compiler stops searching, and the cooker fails to find slang.h.
set -euo pipefail

TIDY_TOOL=tidy
# shellcheck source=lib/tidy-common.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib/tidy-common.sh"

db_arg=""
while [ $# -gt 0 ]; do
    case "$1" in
        -p | --db)
            db_arg="${2:-}"
            if [ -z "$db_arg" ]; then
                tidy_say "$1 needs a build directory."
                exit 2
            fi
            shift 2
            ;;
        --)
            shift
            break
            ;;
        -*)
            tidy_say "unknown option: $1"
            exit 2
            ;;
        *)
            break
            ;;
    esac
done

if [ $# -eq 0 ]; then
    echo "usage: scripts/tidy.sh --db <build-dir> <source> [source ...]" >&2
    echo "       Name the build tree (-p/--db, or \$VENG_TIDY_DB) — a checkout carries" >&2
    echo "       several, and the tree your work is built against is the one to lint" >&2
    echo "       against. Absent that, build-debug/, build/, cmake-build-debug/ are" >&2
    echo "       searched in order and the first that exists wins, which is a guess." >&2
    exit 2
fi

if ! command -v clang-tidy >/dev/null 2>&1; then
    tidy_say "clang-tidy not found on PATH."
    exit 1
fi

tidy_resolve_db "$db_arg" || {
    rc=$?
    if [ "$rc" -eq 2 ]; then
        tidy_say "no usable compile_commands.json in build-debug/, build/, or cmake-build-debug/."
        tidy_note "Name one explicitly with --db <dir> or \$VENG_TIDY_DB."
    fi
    exit 1
}
if [ "$TIDY_DB_EXPLICIT" -eq 0 ]; then
    tidy_say "no build tree named, falling back to $TIDY_DB — pass --db <dir> to be sure this"
    tidy_note "is the tree your work is built against."
fi

# Reject a file the DB carries no entry for. The arguments were named explicitly, so skipping
# one would be misleading and linting it would report nonsense. A source excluded from every
# target (a disabled test, or one built only out of tree) lands here.
for f in "$@"; do
    if [ ! -f "$f" ]; then
        tidy_say "no such file: $f"
        exit 1
    fi
    if ! tidy_db_has_entry "$TIDY_DB" "$f"; then
        tidy_say "$f is in no target, so $TIDY_DB has no flags for it — skipping it would be"
        tidy_note "misleading and linting it would report nonsense. Build it, or drop it"
        tidy_note "from the argument list."
        exit 1
    fi
done

if ! tidy_db_has_sysroot "$TIDY_DB"; then
    tidy_say "$TIDY_DB was configured without CMAKE_OSX_SYSROOT, so its compile DB records no"
    tidy_note "-isysroot and every TU will fail to find the standard headers."
    echo "" >&2
    tidy_note "Configure from scratch — adding the flag to the existing tree does NOT work,"
    tidy_note "because CMake caches the implicit-include list and will keep stripping -I for"
    tidy_note "/usr/local/include while the compiler stops searching it, breaking the cooker:"
    echo "" >&2
    tidy_note "rm -rf $TIDY_DB"
    tidy_note "cmake -B $TIDY_DB -S . -G Ninja -DVE_DEBUG=ON -DCMAKE_OSX_SYSROOT=\"\$(xcrun --show-sdk-path)\""
    exit 1
fi

tidy_export_toolchain_env "$TIDY_DB"
tidy_make_filtered_db "$TIDY_DB"

out=$(clang-tidy -p "$TIDY_FILTERED_DB" --quiet "$@" 2>&1 || true)

if tidy_run_failed "$out"; then
    tidy_say "clang-tidy could not run, so nothing was checked."
    echo "This is a tooling failure, not a clean tree. Cause below." >&2
    echo "------------------------------------------------------------------" >&2
    tidy_run_failure_lines "$out" >&2
    exit 1
fi

if tidy_has_findings "$out"; then
    tidy_finding_lines "$out" >&2
    exit 1
fi

echo "tidy: clean (${#} file(s) checked against $TIDY_DB)."
