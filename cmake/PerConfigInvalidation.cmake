# PerConfigInvalidation.cmake — build-graph regression for the cook's incremental invalidation.
#
# Run via `cmake -P PerConfigInvalidation.cmake -D VENG_BUILD_DIR=<dir> ...`
# (see the `cook_per_config_invalidation` test registration in the root CMakeLists.txt).
#
# Asserts two veng_add_project guarantees through the real build graph, reusing one built tree:
#
#   1. Per-config isolation. Each configuration is its own output set, custom target, and
#      depfile, so editing one configuration re-cooks only its pack. Touch windows.buildcfg,
#      rebuild, and assert the macOS pack is untouched while the Windows pack re-cooked — the
#      macOS pack's DEPENDS/DEPFILE never name the Windows config.
#
#   2. Per-asset source invalidation. The depfile's per-asset edges (a prefab, texture, or
#      shader source) must actually re-trigger the cook. Touch a prefab source, rebuild, and
#      assert the macOS pack re-cooked. This guards the depfile-target/primary-output alignment
#      the Unix Makefiles generator needs: it attaches the cook recipe to the first OUTPUT and
#      the depfile's edges to the depfile's named target, so the two must be the same file (the
#      cooked project leads the OUTPUT list) or a source edit silently never re-cooks.
#
# It builds cook-all-packs (both the macOS/ASTC and Windows/BC7 hello-triangle packs) once and
# reuses it for both checks. ICD-free: a texture cook needs no GPU, so this runs unconditionally.

foreach (VAR VENG_BUILD_DIR VENG_WINDOWS_CONFIG VENG_MACOS_PACK VENG_WINDOWS_PACK VENG_PREFAB_SOURCE)
    if (NOT DEFINED ${VAR})
        message(FATAL_ERROR "cook_per_config_invalidation: ${VAR} not set")
    endif ()
endforeach ()

# build(<what>) — drives the configured build tree's generator at the named target.
function(build TARGET)
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build "${VENG_BUILD_DIR}" --target ${TARGET}
        RESULT_VARIABLE BUILD_RESULT)
    if (NOT BUILD_RESULT EQUAL 0)
        message(FATAL_ERROR "cook_per_config_invalidation: building ${TARGET} exited ${BUILD_RESULT}")
    endif ()
endfunction()

# ---- Cook every configuration's pack ----------------------------------------
build(cook-all-packs)
if (NOT EXISTS "${VENG_MACOS_PACK}" OR NOT EXISTS "${VENG_WINDOWS_PACK}")
    message(FATAL_ERROR "cook_per_config_invalidation: a per-config pack was not cooked")
endif ()

# The two configurations select different codecs (ASTC vs BC7), so the cooked bytes
# differ — proof each configuration actually drove its own cook.
file(SIZE "${VENG_MACOS_PACK}" MACOS_SIZE)
file(SIZE "${VENG_WINDOWS_PACK}" WINDOWS_SIZE)
if (MACOS_SIZE EQUAL WINDOWS_SIZE)
    message(FATAL_ERROR
            "cook_per_config_invalidation: the ASTC and BC7 packs are the same size; config ignored")
endif ()

# ---- Touch the Windows config; rebuild; assert isolation --------------------
file(TIMESTAMP "${VENG_MACOS_PACK}" MACOS_BEFORE "%Y%m%d%H%M%S")
file(TIMESTAMP "${VENG_WINDOWS_PACK}" WINDOWS_BEFORE "%Y%m%d%H%M%S")

# A second of resolution makes a re-cook's timestamp distinguishable.
execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1.1)
file(TOUCH "${VENG_WINDOWS_CONFIG}")

build(cook-all-packs)

file(TIMESTAMP "${VENG_MACOS_PACK}" MACOS_AFTER "%Y%m%d%H%M%S")
file(TIMESTAMP "${VENG_WINDOWS_PACK}" WINDOWS_AFTER "%Y%m%d%H%M%S")

if (NOT MACOS_BEFORE STREQUAL MACOS_AFTER)
    message(FATAL_ERROR
            "cook_per_config_invalidation: editing windows.buildcfg re-cooked the macOS pack")
endif ()
if (WINDOWS_BEFORE STREQUAL WINDOWS_AFTER)
    message(FATAL_ERROR
            "cook_per_config_invalidation: editing windows.buildcfg did not re-cook the Windows pack")
endif ()

message(STATUS "cook_per_config_invalidation: editing one config re-cooks only its pack.")

# ---- Touch a prefab source; rebuild; assert the pack re-cooks ---------------
# A per-asset source reaches the cook only through the depfile, so this catches a
# depfile that is emitted but not honored (the Makefiles depfile-target misalignment).
file(TIMESTAMP "${VENG_MACOS_PACK}" MACOS_SRC_BEFORE "%Y%m%d%H%M%S")

execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1.1)
file(TOUCH "${VENG_PREFAB_SOURCE}")

build(cook-all-packs)

file(TIMESTAMP "${VENG_MACOS_PACK}" MACOS_SRC_AFTER "%Y%m%d%H%M%S")
if (MACOS_SRC_BEFORE STREQUAL MACOS_SRC_AFTER)
    message(FATAL_ERROR
            "cook_per_config_invalidation: editing a prefab source did not re-cook the pack")
endif ()

message(STATUS "cook_per_config_invalidation: editing a per-asset source re-cooks the pack.")
