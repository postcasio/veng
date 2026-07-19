#!/usr/bin/env bash
# Run clang-tidy over the named sources against the repo-root .clang-tidy.
#
#   scripts/tidy.sh engine/src/Gui/Document.cpp [more.cpp ...]
#
# macOS ships no clang-tidy, so the tool is Homebrew LLVM's and it runs as libTooling
# rather than as the compiler driver. libTooling cannot consume the PCH CMake built —
# it is rejected even when the build and the tool are the same LLVM, because the two
# compute different module flags — and a rejected PCH aborts the run before a single
# check evaluates. CMake emits `-include <header>` beside `-include-pch`, so dropping
# the PCH pair leaves the same translation unit, merely uncached. The filtered copy
# lives in a temp dir this run owns; the build's own DB is never touched.
#
# The other half of the problem is fixed at configure time rather than here: the build
# tree is configured with CMAKE_OSX_SYSROOT so the DB records -isysroot. Without it
# every TU dies on 'cstdint' file not found, since the compiler resolves the SDK
# implicitly as a driver and libTooling does not.
#
# The sysroot must be set when the tree is FIRST configured. CMake computes the
# implicit-include list once and caches it, and on this host /usr/local/include — where
# the Vulkan and Slang headers live — is on it, so CMake strips -I for that directory.
# A fresh sysroot configure recomputes the list without it and emits -isystem instead;
# adding the flag to an existing tree leaves the stale list in place, so CMake keeps
# stripping while the compiler stops searching, and the cooker fails to find slang.h.
set -euo pipefail

db_arg="${VENG_TIDY_DB:-}"
while [ $# -gt 0 ]; do
    case "$1" in
        -p | --db)
            db_arg="${2:-}"
            if [ -z "$db_arg" ]; then
                echo "tidy: $1 needs a build directory." >&2
                exit 2
            fi
            shift 2
            ;;
        --)
            shift
            break
            ;;
        -*)
            echo "tidy: unknown option: $1" >&2
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
    echo "tidy: clang-tidy not found on PATH." >&2
    exit 1
fi

# An explicitly named tree is used as given or not at all: falling back to a guess would run
# the lint against a different tree than the one asked for and report the result as if it were
# the one requested. Auto-resolution applies only when no tree is named.
root=$(git rev-parse --show-toplevel 2>/dev/null || pwd)
db=""
if [ -n "$db_arg" ]; then
    if [ ! -f "$db_arg/compile_commands.json" ]; then
        echo "tidy: $db_arg has no compile_commands.json." >&2
        exit 1
    fi
    db="$db_arg"
else
    for d in build-debug build cmake-build-debug; do
        if [ -f "$d/compile_commands.json" ]; then
            home=$(grep -m1 '^CMAKE_HOME_DIRECTORY:' "$d/CMakeCache.txt" 2>/dev/null | cut -d= -f2 || true)
            if [ -n "$home" ] && [ "$home" != "$root" ]; then
                echo "tidy: $d was configured for $home, not $root — skipping it." >&2
                continue
            fi
            db="$d"
            break
        fi
    done
fi
if [ -z "$db" ]; then
    echo "tidy: no usable compile_commands.json in build-debug/, build/, or cmake-build-debug/." >&2
    echo "      Name one explicitly with --db <dir> if it lives elsewhere." >&2
    exit 1
fi
if [ -z "$db_arg" ]; then
    echo "tidy: no build tree named, falling back to $db — pass --db <dir> to be sure this" >&2
    echo "      is the tree your work is built against." >&2
fi

# A tree configured for a different checkout carries entries whose paths no longer resolve, so
# every argument reads as absent from it. Report that as the stale tree it is rather than as a
# file that belongs to no target.
db_home=$(grep -m1 '^CMAKE_HOME_DIRECTORY:' "$db/CMakeCache.txt" 2>/dev/null | cut -d= -f2 || true)
if [ -n "$db_home" ] && [ "$db_home" != "$root" ]; then
    echo "tidy: $db was configured for $db_home, not $root — its entries name paths that do" >&2
    echo "      not exist here, so nothing would be checked. Reconfigure it, or pass a tree" >&2
    echo "      configured for this checkout with --db <dir>." >&2
    exit 1
fi

# Reject a file that is missing, or that the DB carries no entry for. clang-tidy falls back
# to default flags for an unknown file rather than declining it, so the run reports a cascade
# of bogus "use of undeclared identifier" errors that look like a broken tree instead of an
# unlintable argument. A source excluded from every target (a disabled test) lands here.
for f in "$@"; do
    if [ ! -f "$f" ]; then
        echo "tidy: no such file: $f" >&2
        exit 1
    fi
    abs="$(cd "$(dirname "$f")" && pwd)/$(basename "$f")"
    if ! grep -qF "\"$abs\"" "$db/compile_commands.json"; then
        echo "tidy: $f is in no target, so $db has no flags for it — skipping it would be" >&2
        echo "      misleading and linting it would report nonsense. Build it, or drop it" >&2
        echo "      from the argument list." >&2
        exit 1
    fi
done

if ! grep -q -- '-isysroot' "$db/compile_commands.json"; then
    echo "tidy: $db was configured without CMAKE_OSX_SYSROOT, so its compile DB records no" >&2
    echo "      -isysroot and every TU will fail to find the standard headers." >&2
    echo "" >&2
    echo "      Configure from scratch — adding the flag to the existing tree does NOT work," >&2
    echo "      because CMake caches the implicit-include list and will keep stripping -I for" >&2
    echo "      /usr/local/include while the compiler stops searching it, breaking the cooker:" >&2
    echo "" >&2
    echo "      rm -rf $db" >&2
    echo "      cmake -B $db -S . -G Ninja -DVE_DEBUG=ON -DCMAKE_OSX_SYSROOT=\"\$(xcrun --show-sdk-path)\"" >&2
    exit 1
fi

# The Vulkan headers sit on a path the configured compiler searches implicitly, so CMake
# records no -I for them and a backend TU dies on 'vulkan/vulkan.hpp' file not found.
# Supply the configured location by env rather than per-file --extra-arg plumbing.
vk=$(grep -m1 '^Vulkan_INCLUDE_DIR:' "$db/CMakeCache.txt" 2>/dev/null | cut -d= -f2 || true)
[ -n "$vk" ] && export CPATH="${vk}${CPATH:+:$CPATH}"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
sed -e 's/ -Xclang -include-pch -Xclang [^ "]*//g' \
    -e 's/ -Xclang -fno-pch-timestamp//g' \
    -e 's/ -Winvalid-pch//g' \
    "$db/compile_commands.json" > "$tmp/compile_commands.json"

out=$(clang-tidy -p "$tmp" --quiet "$@" 2>&1 || true)

# A toolchain or compile failure prints no findings, which is indistinguishable from a
# clean tree if only warnings are grepped. Fail loudly rather than report a false green.
if printf '%s\n' "$out" | grep -qE '^error:|Error while processing'; then
    echo "tidy: clang-tidy could not run, so nothing was checked." >&2
    echo "This is a tooling failure, not a clean tree. Cause below." >&2
    echo "------------------------------------------------------------------" >&2
    printf '%s\n' "$out" | grep -E '^error:|Error while processing' >&2
    exit 1
fi

if printf '%s\n' "$out" | grep -qE ': (warning|error):'; then
    printf '%s\n' "$out" | grep -E ': (warning|error):' >&2
    exit 1
fi

echo "tidy: clean (${#} file(s) checked against $db)."
