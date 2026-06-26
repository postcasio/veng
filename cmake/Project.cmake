# add_project(<target>
#     PROJECT    <project.veng>            # the authoring project, relative to CMAKE_CURRENT_SOURCE_DIR
#     OUTPUT_DIR <dir>                      # absolute build-tree dir for the cooked packs + .vengproj
#     [MODULE    <lib target>]             # game module to dlopen for prefab/level reflection
#     [REFERENCE <reference pack.json...>] # packs whose ids the cook may resolve
# )
#
# Cooks a project — its asset packs plus a cooked project file (.vengproj) — with
# `vengc cook-project`, one output set per build configuration the project declares.
# The project is the single source of truth: `packs` and `configurations` are read
# from project.veng at configure time, so a new pack or ship target is a project-file
# edit, not a CMake change.
#
# For each configuration vengc cooks every pack into <stem><suffix>.vengpack and writes
# <projstem><suffix>.vengproj naming the packs' un-suffixed mount names + the startup
# level. Each (project x configuration) is its own custom target
# (${TARGET_NAME}-<configname>), registered into cook-all-packs.
#
# The host-default configuration's target is returned in the parent scope as
# ${TARGET_NAME}_HOST_TARGET, and carries the properties veng_add_game / veng_add_editor
# read to copy the artifacts beside the launcher and to bake the editor's project path:
#   VENG_PROJECT_OUTPUT  the cooked .vengproj path (suffixed) in the build tree
#   VENG_PROJECT_MOUNT   the un-suffixed .vengproj name the runtime loads
#   VENG_PACK_OUTPUTS    the cooked pack paths (suffixed), parallel to VENG_PACK_MOUNTS
#   VENG_PACK_MOUNTS     the un-suffixed pack names the runtime mounts
#   VENG_PROJECT_SOURCE  the absolute project.veng source path (for the editor)
function(add_project TARGET_NAME)
    cmake_parse_arguments(ARG "" "PROJECT;OUTPUT_DIR;MODULE" "REFERENCE" ${ARGN})

    if (NOT ARG_PROJECT)
        message(FATAL_ERROR "add_project(${TARGET_NAME}): PROJECT is required")
    endif ()
    if (NOT ARG_OUTPUT_DIR)
        message(FATAL_ERROR "add_project(${TARGET_NAME}): OUTPUT_DIR is required")
    endif ()

    cmake_path(ABSOLUTE_PATH ARG_PROJECT
            BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} NORMALIZE
            OUTPUT_VARIABLE PROJECT_ABS)
    if (NOT EXISTS ${PROJECT_ABS})
        message(FATAL_ERROR "add_project(${TARGET_NAME}): PROJECT '${PROJECT_ABS}' not found")
    endif ()
    cmake_path(GET PROJECT_ABS PARENT_PATH PROJECT_DIR)
    cmake_path(GET PROJECT_ABS STEM LAST_ONLY PROJECT_STEM)

    file(READ ${PROJECT_ABS} PROJECT_JSON)

    set(REFERENCE_ARGS)
    foreach (REF IN LISTS ARG_REFERENCE)
        cmake_path(ABSOLUTE_PATH REF
                BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} NORMALIZE
                OUTPUT_VARIABLE REF_ABS)
        list(APPEND REFERENCE_ARGS --reference ${REF_ABS})
    endforeach ()

    set(MODULE_ARGS)
    set(MODULE_DEP)
    if (ARG_MODULE)
        set(MODULE_ARGS --module $<TARGET_FILE:${ARG_MODULE}>)
        set(MODULE_DEP ${ARG_MODULE})
    endif ()

    # The packs the project owns, resolved to mount names + per-config output stems.
    string(JSON PACK_COUNT ERROR_VARIABLE PACK_ERR LENGTH ${PROJECT_JSON} packs)
    if (PACK_ERR OR PACK_COUNT EQUAL 0)
        message(FATAL_ERROR "add_project(${TARGET_NAME}): project '${PROJECT_ABS}' lists no packs")
    endif ()

    set(PACK_MANIFESTS)  # absolute pack JSON sources
    set(PACK_STEMS)      # e.g. template (the part before .vengpack)
    set(PACK_MOUNTS)     # e.g. template.vengpack (the runtime mount name)
    math(EXPR PACK_LAST "${PACK_COUNT} - 1")
    foreach (i RANGE 0 ${PACK_LAST})
        string(JSON PACK_REL GET ${PROJECT_JSON} packs ${i})
        cmake_path(ABSOLUTE_PATH PACK_REL BASE_DIRECTORY ${PROJECT_DIR} NORMALIZE
                OUTPUT_VARIABLE PACK_ABS)
        # template.vengpack.json -> mount template.vengpack -> stem template
        cmake_path(GET PACK_ABS STEM LAST_ONLY PACK_MOUNT)
        set(PACK_MOUNT_PATH ${PACK_MOUNT})
        cmake_path(GET PACK_MOUNT_PATH STEM LAST_ONLY PACK_STEM)
        list(APPEND PACK_MANIFESTS ${PACK_ABS})
        list(APPEND PACK_STEMS ${PACK_STEM})
        list(APPEND PACK_MOUNTS ${PACK_MOUNT})
    endforeach ()

    # The configurations the project ships; each cooks its own output set.
    string(JSON CFG_COUNT ERROR_VARIABLE CFG_ERR LENGTH ${PROJECT_JSON} configurations)
    if (CFG_ERR OR CFG_COUNT EQUAL 0)
        message(FATAL_ERROR
                "add_project(${TARGET_NAME}): project '${PROJECT_ABS}' lists no configurations")
    endif ()

    math(EXPR CFG_LAST "${CFG_COUNT} - 1")
    foreach (c RANGE 0 ${CFG_LAST})
        string(JSON CFG_REL GET ${PROJECT_JSON} configurations ${c})
        cmake_path(ABSOLUTE_PATH CFG_REL BASE_DIRECTORY ${PROJECT_DIR} NORMALIZE
                OUTPUT_VARIABLE CFG_ABS)
        if (NOT EXISTS ${CFG_ABS})
            message(FATAL_ERROR
                    "add_project(${TARGET_NAME}): configuration '${CFG_ABS}' not found")
        endif ()

        # The configuration's name (the --config selector) and pack suffix come from the
        # *.buildcfg, the single source of truth — read them rather than re-declaring them.
        file(READ ${CFG_ABS} CFG_JSON)
        string(JSON CFG_NAME ERROR_VARIABLE NAME_ERR GET ${CFG_JSON} name)
        string(JSON CFG_SUFFIX ERROR_VARIABLE SUFFIX_ERR GET ${CFG_JSON} outputSuffix)
        if (NAME_ERR OR SUFFIX_ERR)
            message(FATAL_ERROR
                    "add_project(${TARGET_NAME}): config '${CFG_ABS}' needs string name + outputSuffix")
        endif ()

        # The per-config outputs: each pack (suffixed) plus the cooked project (suffixed).
        set(CFG_OUTPUTS)
        set(CFG_PACK_OUTPUTS)
        foreach (PACK_STEM IN LISTS PACK_STEMS)
            set(PACK_OUT ${ARG_OUTPUT_DIR}/${PACK_STEM}${CFG_SUFFIX}.vengpack)
            list(APPEND CFG_OUTPUTS ${PACK_OUT})
            list(APPEND CFG_PACK_OUTPUTS ${PACK_OUT})
        endforeach ()
        set(PROJ_OUT ${ARG_OUTPUT_DIR}/${PROJECT_STEM}${CFG_SUFFIX}.vengproj)
        list(APPEND CFG_OUTPUTS ${PROJ_OUT})

        add_custom_command(
                OUTPUT ${CFG_OUTPUTS}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${ARG_OUTPUT_DIR}
                COMMAND $<TARGET_FILE:vengc> cook-project ${PROJECT_ABS}
                        --config ${CFG_NAME} --out-dir ${ARG_OUTPUT_DIR}
                        ${MODULE_ARGS} ${REFERENCE_ARGS} --depfile ${PROJ_OUT}.d
                DEPENDS vengc ${PROJECT_ABS} ${CFG_ABS} ${PACK_MANIFESTS} ${MODULE_DEP}
                DEPFILE ${PROJ_OUT}.d
                COMMENT "Cooking project ${TARGET_NAME} (${CFG_NAME})")

        set(CFG_TARGET ${TARGET_NAME}-${CFG_NAME})
        add_custom_target(${CFG_TARGET} DEPENDS ${CFG_OUTPUTS})
        veng_register_all_packs_target(${CFG_TARGET})

        # Every config target carries its own cooked-output paths (the per-config
        # invalidation test reads VENG_PACK_OUTPUTS off a named config target); the
        # mount names + project source it shares are read from the host target only.
        set_target_properties(${CFG_TARGET} PROPERTIES
                VENG_PROJECT_OUTPUT ${PROJ_OUT}
                VENG_PROJECT_MOUNT ${PROJECT_STEM}.vengproj
                VENG_PACK_OUTPUTS "${CFG_PACK_OUTPUTS}"
                VENG_PACK_MOUNTS "${PACK_MOUNTS}"
                VENG_PROJECT_SOURCE ${PROJECT_ABS})

        if (CFG_NAME STREQUAL VENG_BUILD_CONFIG)
            set(${TARGET_NAME}_HOST_TARGET ${CFG_TARGET} PARENT_SCOPE)
        endif ()
    endforeach ()
endfunction()
