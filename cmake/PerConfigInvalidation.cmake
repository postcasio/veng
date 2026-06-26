# PerConfigInvalidation.cmake — build-graph regression for per-config cook isolation.
#
# Run via `cmake -P PerConfigInvalidation.cmake -D VENG_BUILD_DIR=<dir> ...`
# (see the `cook_per_config_invalidation` test registration in the root CMakeLists.txt).
#
# Asserts add_asset_pack's central guarantee through the real build graph: each
# (pack x configuration) is its own output, custom target, and depfile, so editing one
# configuration re-cooks only its pack. It builds cook-all-packs (both the macOS/ASTC
# and Windows/BC7 hello-triangle packs), records the macOS pack's timestamp, touches
# windows.buildcfg, rebuilds cook-all-packs, and asserts the macOS pack is untouched
# while the Windows pack was re-cooked — the macOS pack's DEPENDS/DEPFILE never name
# the Windows config. ICD-free: a texture cook needs no GPU, so this runs unconditionally.

foreach (VAR VENG_BUILD_DIR VENG_WINDOWS_CONFIG VENG_MACOS_PACK VENG_WINDOWS_PACK)
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
