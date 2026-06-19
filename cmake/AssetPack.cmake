# add_asset_pack(<target>
#     PACK      <pack.json>                # relative to CMAKE_CURRENT_SOURCE_DIR
#     OUTPUT    <out.vengpack>             # absolute path in the build tree
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
function(add_asset_pack TARGET_NAME)
    cmake_parse_arguments(ARG "" "PACK;OUTPUT;MODULE" "DEPENDS;REFERENCE" ${ARGN})

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

    cmake_path(GET ARG_OUTPUT PARENT_PATH OUTPUT_DIR)

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
    add_custom_command(
            OUTPUT ${ARG_OUTPUT}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${OUTPUT_DIR}
            COMMAND $<TARGET_FILE:vengc> cook ${PACK_ABS} -o ${ARG_OUTPUT} ${REFERENCE_ARGS} ${MODULE_ARGS} --depfile ${ARG_OUTPUT}.d
            DEPENDS vengc ${PACK_ABS} ${ARG_DEPENDS} ${MODULE_DEP}
            DEPFILE ${ARG_OUTPUT}.d
            COMMENT "Cooking asset pack ${TARGET_NAME}")

    add_custom_target(${TARGET_NAME} DEPENDS ${ARG_OUTPUT})
    # Record the cooked file's path so veng_add_game can copy it beside the
    # launcher without the caller restating it, and the source manifest path so
    # the editor can resolve an AssetId to its per-asset JSON source for editing.
    set_target_properties(${TARGET_NAME} PROPERTIES
            VENG_ASSET_PACK_OUTPUT ${ARG_OUTPUT}
            VENG_ASSET_PACK_SOURCE ${PACK_ABS})
endfunction()
