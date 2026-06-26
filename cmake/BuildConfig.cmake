# Build-configuration selection for the asset cook.
#
# A project ships a set of build configurations (one per ship target: macOS / Windows /
# Linux …), each a `*.buildcfg` holding the texture role -> format codec policy. This
# module decides *which* configuration a build cooks: VENG_BUILD_CONFIG, a cache
# variable whose default is derived from the host triple, so a bare `cmake --build` on
# a host cooks the host-matching configuration with no flag. An explicit
# `-DVENG_BUILD_CONFIG=<name>` overrides (any configuration cooks on any host — the
# encoder is CPU). The configuration *set* is the project's; the engine provides only
# the selection variable, the host-triple default, and the cook-all aggregate.

# veng_host_default_config_name(<out-var>)
#
# Maps the host triple (CMAKE_HOST_SYSTEM_NAME + CMAKE_HOST_SYSTEM_PROCESSOR) to a
# canonical short configuration name — "macos", "windows", or "linux" — the natural
# default for the host. A project uses it to default VENG_BUILD_CONFIG and to pick the
# matching `*.buildcfg`. An unrecognized host falls back to "linux", the broad default.
function(veng_host_default_config_name OUT_VAR)
    if (CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
        set(NAME "macos")
    elseif (CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        set(NAME "windows")
    else ()
        set(NAME "linux")
    endif ()
    set(${OUT_VAR} ${NAME} PARENT_SCOPE)
endfunction()

# The default cooked configuration: the host-matching one. Sticky once set in a build
# tree — a plain `cmake --build` keeps cooking whatever this last resolved to until
# re-set or the cache is cleared.
veng_host_default_config_name(VENG_BUILD_CONFIG_DEFAULT)
set(VENG_BUILD_CONFIG "${VENG_BUILD_CONFIG_DEFAULT}" CACHE STRING
    "Build configuration the asset cook selects (host-matching by default)")

# Surface the resolved selection at configure time so a foreign override is never
# silent: a stale cross-platform build shows on the configure line, not as a passing
# test mounting an unexpected pack.
message(STATUS "veng: asset cook build configuration = ${VENG_BUILD_CONFIG}")

# The cook-all-packs aggregate depends on every configuration's output pack, for CI /
# ship that wants all platforms' packs in one build. add_asset_pack feeds each
# per-config pack target into it via veng_register_all_packs_target; the default build
# still cooks only the host-default pack, so a developer's incremental build never pays
# for foreign-platform encodes it does not need.
if (NOT TARGET cook-all-packs)
    add_custom_target(cook-all-packs)
endif ()

# veng_register_all_packs_target(<pack target>)
#
# Adds a per-config pack target as a dependency of cook-all-packs, so building that
# aggregate cooks the pack. A project calls it for each configuration it ships beyond
# the host default.
function(veng_register_all_packs_target PACK_TARGET)
    add_dependencies(cook-all-packs ${PACK_TARGET})
endfunction()
