# add_asset_pack(<target>
#     PACK      <pack.json>                # relative to CMAKE_CURRENT_SOURCE_DIR
#     OUTPUT    <out.vengpack>             # absolute path in the build tree
#     [CONFIG    <file.buildcfg>]          # build configuration driving the cook
#     [DEPENDS   <source files...>]        # optional extra re-cook triggers
#     [REFERENCE <reference pack.json...>] # packs whose ids the cook may resolve
#     [MODULE    <lib target>]             # game module to dlopen for prefab reflection
# )
#
# Cooks a JSON asset pack with vengc into OUTPUT and wraps it in a custom target:
# a build-time tool invocation producing an artifact in the build tree. vengc
# emits a depfile of every source it actually read (the manifest, references,
# per-asset JSONs, and their binary payloads — images, models, shader sources
# and includes), wired via DEPFILE, so a re-cook triggers when any of them
# changes without restating them. DEPENDS is an optional manual supplement for
# inputs outside the cook's view; it is not needed for the pack's own sources.
# REFERENCE entries become `--reference` flags so a shader's vertex-layout id (or
# any cross-pack id) resolves against an already-authored pack (e.g. the engine
# core pack).
# MODULE names a game library target whose native component types a prefab entry
# cooks against: the cook gains `--module $<TARGET_FILE:lib>` and the custom
# command DEPENDS on the lib, so the build graph adds `lib -> cook`. Packs with no
# MODULE are independent of any lib.
#
# CONFIG names a `*.buildcfg` build configuration. Its OutputSuffix (read from the
# file, the single source of truth) is appended to the output filename, so each
# (pack x configuration) is its own output, custom target (${TARGET_NAME}-<suffix>),
# and depfile — editing one configuration re-cooks only its pack. The cook gains
# `--config <file>` and the file joins DEPENDS so the first build (before a depfile
# exists) still re-cooks on a configuration edit. With no CONFIG the pack cooks the
# zero-config default into the un-suffixed name and the bare ${TARGET_NAME}.
#
# The resolved custom-target name (suffixed under CONFIG, bare without) is returned
# in the parent scope as ${TARGET_NAME}_TARGET, so a caller threading the
# host-default CONFIG does not need to know the suffix to depend on the target.
function(add_asset_pack TARGET_NAME)
    cmake_parse_arguments(ARG "" "PACK;OUTPUT;MODULE;CONFIG" "DEPENDS;REFERENCE" ${ARGN})

    if (NOT ARG_PACK)
        message(FATAL_ERROR "add_asset_pack(${TARGET_NAME}): PACK is required")
    endif ()
    if (NOT ARG_OUTPUT)
        message(FATAL_ERROR "add_asset_pack(${TARGET_NAME}): OUTPUT is required")
    endif ()

    cmake_path(ABSOLUTE_PATH ARG_PACK
            BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} NORMALIZE
            OUTPUT_VARIABLE PACK_ABS)

    set(REFERENCE_ARGS)
    foreach (REF IN LISTS ARG_REFERENCE)
        cmake_path(ABSOLUTE_PATH REF
                BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} NORMALIZE
                OUTPUT_VARIABLE REF_ABS)
        list(APPEND REFERENCE_ARGS --reference ${REF_ABS})
    endforeach ()

    # A CONFIG pack reads OutputSuffix from the configuration file and derives a
    # per-config output / target / depfile from it, so configurations never collide.
    # The mount name is the un-suffixed filename the app loads (veng_add_game renames
    # the suffixed build artifact to it beside the launcher). With no CONFIG the
    # output is the un-suffixed path verbatim — the zero-config default.
    set(CONFIG_ARGS)
    set(CONFIG_DEP)
    set(TARGET_SUFFIX)
    cmake_path(GET ARG_OUTPUT FILENAME MOUNT_NAME)
    set(PACK_OUTPUT ${ARG_OUTPUT})
    if (ARG_CONFIG)
        cmake_path(ABSOLUTE_PATH ARG_CONFIG
                BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} NORMALIZE
                OUTPUT_VARIABLE CONFIG_ABS)
        if (NOT EXISTS ${CONFIG_ABS})
            message(FATAL_ERROR "add_asset_pack(${TARGET_NAME}): CONFIG '${CONFIG_ABS}' not found")
        endif ()

        # OutputSuffix in the configuration file is the single source of truth for the
        # per-config suffix; read it from there rather than re-declaring it as an argument.
        file(READ ${CONFIG_ABS} CONFIG_JSON)
        string(JSON CONFIG_SUFFIX ERROR_VARIABLE SUFFIX_ERROR GET ${CONFIG_JSON} "outputSuffix")
        if (SUFFIX_ERROR)
            message(FATAL_ERROR
                    "add_asset_pack(${TARGET_NAME}): CONFIG '${CONFIG_ABS}' has no string outputSuffix")
        endif ()

        # Splice the suffix in before the extension: sample.vengpack -> sample-macos.vengpack.
        cmake_path(GET ARG_OUTPUT STEM LAST_ONLY OUTPUT_STEM)
        cmake_path(GET ARG_OUTPUT EXTENSION LAST_ONLY OUTPUT_EXT)
        cmake_path(GET ARG_OUTPUT PARENT_PATH OUTPUT_PARENT)
        set(PACK_OUTPUT ${OUTPUT_PARENT}/${OUTPUT_STEM}${CONFIG_SUFFIX}${OUTPUT_EXT})

        set(CONFIG_ARGS --config ${CONFIG_ABS})
        set(CONFIG_DEP ${CONFIG_ABS})
        # Strip the leading '-' so the target reads hello_triangle_assets-macos.
        string(REGEX REPLACE "^-" "" CONFIG_TAG "${CONFIG_SUFFIX}")
        set(TARGET_SUFFIX "-${CONFIG_TAG}")
    endif ()

    set(PACK_TARGET ${TARGET_NAME}${TARGET_SUFFIX})
    cmake_path(GET PACK_OUTPUT PARENT_PATH OUTPUT_DIR)

    # A MODULE pack reflects the game module's native types: pass the built lib to
    # vengc and order the cook after the lib (lib -> cook).
    set(MODULE_ARGS)
    set(MODULE_DEP)
    if (ARG_MODULE)
        set(MODULE_ARGS --module $<TARGET_FILE:${ARG_MODULE}>)
        set(MODULE_DEP ${ARG_MODULE})
    endif ()

    # vengc writes a depfile naming every source it read (the manifest, each
    # reference pack, the per-asset JSONs, and their binary payloads — images,
    # models, shader sources and their includes). DEPFILE feeds it to the build
    # so a re-cook triggers when any of them changes, with no hand-maintained
    # DEPENDS list to drift. (ARG_DEPENDS remains an optional manual supplement.)
    # The engine core shader dir is on every cook's Slang search path so a consumer
    # shader resolves `#include "Veng/material.slang"`. A source-dir include still wins.
    add_custom_command(
            OUTPUT ${PACK_OUTPUT}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${OUTPUT_DIR}
            COMMAND $<TARGET_FILE:vengc> cook ${PACK_ABS} -o ${PACK_OUTPUT} ${REFERENCE_ARGS} ${MODULE_ARGS} ${CONFIG_ARGS} --shader-include ${VENG_CORE_SHADER_DIR} --depfile ${PACK_OUTPUT}.d
            DEPENDS vengc ${PACK_ABS} ${ARG_DEPENDS} ${MODULE_DEP} ${CONFIG_DEP}
            DEPFILE ${PACK_OUTPUT}.d
            COMMENT "Cooking asset pack ${PACK_TARGET}")

    add_custom_target(${PACK_TARGET} DEPENDS ${PACK_OUTPUT})
    # Record the cooked (suffixed) file's path so veng_add_game can copy it beside
    # the launcher without the caller restating it; the un-suffixed mount name so
    # that copy renames the per-config artifact to the name the app loads; and the
    # source manifest path so the editor can resolve an AssetId to its per-asset
    # JSON source for editing.
    set_target_properties(${PACK_TARGET} PROPERTIES
            VENG_ASSET_PACK_OUTPUT ${PACK_OUTPUT}
            VENG_ASSET_PACK_MOUNT_NAME ${MOUNT_NAME}
            VENG_ASSET_PACK_SOURCE ${PACK_ABS})

    set(${TARGET_NAME}_TARGET ${PACK_TARGET} PARENT_SCOPE)
endfunction()
