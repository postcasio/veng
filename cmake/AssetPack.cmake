# add_asset_pack(<target>
#     PACK      <pack.json>                # relative to CMAKE_CURRENT_SOURCE_DIR
#     OUTPUT    <out.vengpack>             # absolute path in the build tree
#     [DEPENDS   <source files...>]        # re-cook triggers (per-asset sources + binaries)
#     [REFERENCE <reference pack.json...>] # packs whose ids the cook may resolve
# )
#
# Cooks a JSON asset pack with vengc into OUTPUT and wraps it in a custom target:
# a build-time tool invocation producing an artifact in the build tree. Re-cooks
# when PACK, vengc, or any DEPENDS source changes.
# REFERENCE entries become `--reference` flags so a shader's vertex-layout id (or
# any cross-pack id) resolves against an already-authored pack (e.g. the engine
# core pack).
function(add_asset_pack TARGET_NAME)
    cmake_parse_arguments(ARG "" "PACK;OUTPUT" "DEPENDS;REFERENCE" ${ARGN})

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

    add_custom_command(
            OUTPUT ${ARG_OUTPUT}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${OUTPUT_DIR}
            COMMAND $<TARGET_FILE:vengc> cook ${PACK_ABS} -o ${ARG_OUTPUT} ${REFERENCE_ARGS}
            DEPENDS vengc ${PACK_ABS} ${ARG_DEPENDS}
            COMMENT "Cooking asset pack ${TARGET_NAME}")

    add_custom_target(${TARGET_NAME} DEPENDS ${ARG_OUTPUT})
    # Record the cooked file's path so veng_add_game can copy it beside the
    # launcher without the caller restating it.
    set_target_properties(${TARGET_NAME} PROPERTIES VENG_ASSET_PACK_OUTPUT ${ARG_OUTPUT})
endfunction()
