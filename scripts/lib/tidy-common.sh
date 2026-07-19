# Shared clang-tidy plumbing for scripts/tidy.sh and .githooks/pre-commit.
#
# Source it; it defines functions and runs nothing on its own. A caller sets TIDY_TOOL to the
# name its diagnostics are prefixed with ("tidy", "pre-commit") before sourcing or calling.
#
# The two callers differ in what they do with an unlintable argument — the script refuses,
# because its files were named explicitly and skipping one would mislead; the hook filters,
# because its files come from what was staged and refusing would block the commit. So the
# shared half is everything up to that decision: resolving a build tree, proving the tree
# belongs to this checkout, supplying the flags libTooling needs, and telling a run that
# found nothing apart from a run that never started.

TIDY_TOOL="${TIDY_TOOL:-tidy}"

tidy_say() { echo "${TIDY_TOOL}: $*" >&2; }
tidy_note() { echo "      $*" >&2; }

# The source directory a build tree was configured for; empty if it has no readable cache.
tidy_db_home() {
    grep -m1 '^CMAKE_HOME_DIRECTORY:' "$1/CMakeCache.txt" 2>/dev/null | cut -d= -f2 || true
}

# This checkout's root, or the working directory when git is unavailable.
tidy_repo_root() {
    git rev-parse --show-toplevel 2>/dev/null || pwd
}

# Resolve the build tree whose compile DB is linted against, into TIDY_DB. $1 is a tree named
# on the command line, falling back to $VENG_TIDY_DB, then to the search. TIDY_DB_EXPLICIT
# records which of those happened, so a caller can say when it guessed.
#
# A named tree is used as given or not at all: falling back to the search would lint against a
# different tree than the one asked for and report the result as if it were the one requested.
#
# Returns 0 on success, 1 when a named tree is unusable, and 2 when the search found none. The
# two failures are distinct because a clone that has never been built has no tree through no
# fault of the caller — the hook skips its lint stage on 2 and refuses on 1.
tidy_resolve_db() {
    local explicit="${1:-}"
    [ -n "$explicit" ] || explicit="${VENG_TIDY_DB:-}"
    local root
    root=$(tidy_repo_root)
    TIDY_DB=""
    TIDY_DB_EXPLICIT=0

    if [ -n "$explicit" ]; then
        if [ ! -f "$explicit/compile_commands.json" ]; then
            tidy_say "$explicit has no compile_commands.json."
            return 1
        fi
        local home
        home=$(tidy_db_home "$explicit")
        if [ -n "$home" ] && [ "$home" != "$root" ]; then
            tidy_say "$explicit was configured for $home, not $root — its entries name paths"
            tidy_note "that do not exist here, so nothing would be checked. Reconfigure it, or"
            tidy_note "name a tree configured for this checkout."
            return 1
        fi
        TIDY_DB="$explicit"
        TIDY_DB_EXPLICIT=1
        return 0
    fi

    # build-debug first: it is the tree the project configures and builds by default, where
    # build/ is the optional validation-OFF tree and is routinely absent or stale.
    local d home
    for d in build-debug build cmake-build-debug; do
        [ -f "$d/compile_commands.json" ] || continue
        home=$(tidy_db_home "$d")
        if [ -n "$home" ] && [ "$home" != "$root" ]; then
            tidy_say "$d was configured for $home, not $root — skipping it."
            continue
        fi
        TIDY_DB="$d"
        return 0
    done

    return 2
}

# The Vulkan headers sit on a path the configured compiler searches implicitly, so CMake
# records no -I for them and a backend TU dies on 'vulkan/vulkan.hpp' file not found. Supply
# the configured location by env rather than per-file --extra-arg plumbing. SDKROOT covers the
# same gap for the macOS SDK; both are no-ops off macOS.
tidy_export_toolchain_env() {
    local db="$1" sdk vk
    sdk=$(xcrun --show-sdk-path 2>/dev/null || true)
    [ -n "$sdk" ] && export SDKROOT="$sdk"
    vk=$(grep -m1 '^Vulkan_INCLUDE_DIR:' "$db/CMakeCache.txt" 2>/dev/null | cut -d= -f2 || true)
    [ -n "$vk" ] && export CPATH="${vk}${CPATH:+:$CPATH}"
    return 0
}

# True when the build tree records -isysroot. Without it every TU dies on 'cstdint' file not
# found, since the compiler resolves the SDK implicitly as a driver where libTooling does not.
tidy_db_has_sysroot() {
    grep -q -- '-isysroot' "$1/compile_commands.json"
}

# Write a PCH-stripped copy of the DB to a temp dir this run owns, into TIDY_FILTERED_DB.
#
# libTooling cannot consume the PCH CMake built — it is rejected even when the build and the
# tool are the same LLVM, because the two compute different module flags — and a rejected PCH
# aborts the run before a single check evaluates. CMake emits `-include <header>` beside
# `-include-pch`, so dropping the PCH pair leaves the same translation unit, merely uncached.
#
# Registers the EXIT trap that removes the temp dir; a caller that needs its own EXIT trap must
# chain to tidy_cleanup itself.
tidy_make_filtered_db() {
    local db="$1"
    TIDY_TMP=$(mktemp -d)
    trap tidy_cleanup EXIT
    sed -e 's/ -Xclang -include-pch -Xclang [^ "]*//g' \
        -e 's/ -Xclang -fno-pch-timestamp//g' \
        -e 's/ -Winvalid-pch//g' \
        "$db/compile_commands.json" > "$TIDY_TMP/compile_commands.json"
    TIDY_FILTERED_DB="$TIDY_TMP"
}

tidy_cleanup() {
    [ -n "${TIDY_TMP:-}" ] && rm -rf "$TIDY_TMP"
    return 0
}

# True when the DB carries a compile command for this source. clang-tidy lints a file it has no
# entry for against default flags rather than declining it, so the run reports a cascade of
# bogus errors that reads as a broken tree instead of an unlintable argument — and in a batch,
# that failure poisons the files that follow it.
tidy_db_has_entry() {
    local db="$1" f="$2" abs
    [ -f "$f" ] || return 1
    abs="$(cd "$(dirname "$f")" && pwd)/$(basename "$f")"
    grep -qF "\"$abs\"" "$db/compile_commands.json"
}

# True when the output shows a run that never evaluated a check. A toolchain or compile failure
# prints no findings, which is indistinguishable from a clean tree if only warnings are grepped.
tidy_run_failed() {
    printf '%s\n' "$1" | grep -qE '^error:|Error while processing'
}

tidy_run_failure_lines() {
    printf '%s\n' "$1" | grep -E '^error:|Error while processing'
}

tidy_has_findings() {
    printf '%s\n' "$1" | grep -qE ': (warning|error):'
}

tidy_finding_lines() {
    printf '%s\n' "$1" | grep -E ': (warning|error):'
}
