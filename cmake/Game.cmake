# veng_add_game(<name>
#     SOURCES   <game sources...>        # the libgame translation units
#     [PROJECT  <add_project host target>] # optional: the project's host-config target
# )
#
# Produces lib<name> (SHARED, links veng::veng) and <name>-launcher (exe, the veng
# launcher compiled with VENG_GAME_MODULE pointing at lib<name>).
#
# A PROJECT target (from add_project) declares MODULE <name> so its cook reflects
# lib<name>'s native component types and the build graph orders lib<name> -> cook ->
# copy beside launcher. The cooked project file and every pack it names are copied
# beside the launcher (renamed to their un-suffixed mount names), so the relocatable
# set (launcher + lib + project + packs) is a self-contained, movable directory.
#
# In-tree only: it serves the example and the tests, which build veng as the top-level
# project. Installed-package wiring (resolving the launcher source from an installed
# veng) is a separate packaging concern, out of scope here.

# VENG_LAUNCHER_MAIN is the launcher source. In-tree it is the source-tree path,
# set right here.
set(VENG_LAUNCHER_MAIN "${CMAKE_CURRENT_LIST_DIR}/../engine/src/Launcher/launcher_main.cpp"
    CACHE INTERNAL "veng launcher source")

function(veng_add_game NAME)
    cmake_parse_arguments(ARG "" "PROJECT" "SOURCES" ${ARGN})

    add_library(${NAME} SHARED ${ARG_SOURCES})
    target_link_libraries(${NAME} PRIVATE veng::veng)

    add_executable(${NAME}-launcher ${VENG_LAUNCHER_MAIN})
    target_link_libraries(${NAME}-launcher PRIVATE veng::veng)
    # The launcher dlopens the module by file name; resolve it beside the launcher
    # binary so the pair is relocatable (build tree and a shipped directory both work).
    target_compile_definitions(${NAME}-launcher PRIVATE
        VENG_GAME_MODULE="$<TARGET_FILE_NAME:${NAME}>")

    # Resolve the dlopen'd module beside the launcher binary. BUILD_RPATH is APPENDED
    # to CMake's auto-computed build rpath (it does not replace it), so the launcher
    # still finds libveng via the auto rpath while gaining @loader_path/$ORIGIN for the
    # game module. INSTALL_RPATH is set too (it DOES replace) so the same relative
    # resolution survives an install.
    if (APPLE)
        set(GAME_RPATH "@loader_path")
    else ()
        set(GAME_RPATH "$ORIGIN")
    endif ()
    set_target_properties(${NAME}-launcher PROPERTIES
        BUILD_RPATH   "${GAME_RPATH}"
        INSTALL_RPATH "${GAME_RPATH}")

    # Place lib<name> beside the launcher so @loader_path/$ORIGIN finds it.
    set_target_properties(${NAME} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:${NAME}-launcher>)
    add_dependencies(${NAME}-launcher ${NAME})

    # Copy the cooked project file and every pack it names beside the launcher so
    # ExecutableDirectory()-relative loading finds them; the set (launcher + lib +
    # project + packs) is then a self-contained, movable directory. The host-config
    # project target records the cooked outputs (possibly suffixed per build
    # configuration) and the un-suffixed mount names the runtime loads, both set by
    # add_project, so this copy reads them without the caller restating the paths. Each
    # copy renames source -> mount name, so the per-config suffix never leaves the build
    # tree and the launcher loads the same names whichever config cooked them.
    #
    # Each copy is its own output-producing command keyed on its cooked artifact, not a
    # POST_BUILD step on the launcher: a re-cook (e.g. a prefab edit) does not relink the
    # launcher, so a POST_BUILD copy would never fire and the launcher would load a stale
    # artifact. Depending on the cooked file makes the copy re-run whenever it changes.
    if (ARG_PROJECT)
        add_dependencies(${NAME}-launcher ${ARG_PROJECT})

        # The launcher sets no custom RUNTIME_OUTPUT_DIRECTORY, so it lands in this
        # directory's binary dir; the copies target the same dir. A target-dependent genex
        # is not permitted in a custom command OUTPUT, so destinations are configure-time paths.
        get_target_property(PROJECT_OUTPUT ${ARG_PROJECT} VENG_PROJECT_OUTPUT)
        get_target_property(PROJECT_MOUNT ${ARG_PROJECT} VENG_PROJECT_MOUNT)
        get_target_property(PACK_OUTPUTS ${ARG_PROJECT} VENG_PACK_OUTPUTS)
        get_target_property(PACK_MOUNTS ${ARG_PROJECT} VENG_PACK_MOUNTS)

        set(COPIED_BESIDE_LAUNCHER)

        set(PROJECT_BESIDE_LAUNCHER ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_MOUNT})
        add_custom_command(
            OUTPUT ${PROJECT_BESIDE_LAUNCHER}
            COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_OUTPUT} ${PROJECT_BESIDE_LAUNCHER}
            DEPENDS ${PROJECT_OUTPUT}
            COMMENT "Copying cooked project beside ${NAME}-launcher")
        list(APPEND COPIED_BESIDE_LAUNCHER ${PROJECT_BESIDE_LAUNCHER})

        # Zip the parallel output/mount lists to copy each pack to its mount name.
        list(LENGTH PACK_OUTPUTS PACK_N)
        math(EXPR PACK_LAST "${PACK_N} - 1")
        foreach (i RANGE 0 ${PACK_LAST})
            list(GET PACK_OUTPUTS ${i} PACK_OUTPUT)
            list(GET PACK_MOUNTS ${i} PACK_MOUNT)
            set(PACK_BESIDE_LAUNCHER ${CMAKE_CURRENT_BINARY_DIR}/${PACK_MOUNT})
            add_custom_command(
                OUTPUT ${PACK_BESIDE_LAUNCHER}
                COMMAND ${CMAKE_COMMAND} -E copy ${PACK_OUTPUT} ${PACK_BESIDE_LAUNCHER}
                DEPENDS ${PACK_OUTPUT}
                COMMENT "Copying asset pack beside ${NAME}-launcher")
            list(APPEND COPIED_BESIDE_LAUNCHER ${PACK_BESIDE_LAUNCHER})
        endforeach ()

        add_custom_target(${NAME}-launcher-pack DEPENDS ${COPIED_BESIDE_LAUNCHER})
        add_dependencies(${NAME}-launcher ${NAME}-launcher-pack)
    endif ()

    # Windows has no rpath: the @loader_path/$ORIGIN resolution above is a no-op, so the
    # launcher's dependent DLLs (libveng, …) must sit beside it. Copy them post-build so
    # the trio (launcher + module + pack + DLLs) is a self-contained, runnable directory.
    if (WIN32)
        add_custom_command(TARGET ${NAME}-launcher POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${NAME}-launcher> $<TARGET_FILE_DIR:${NAME}-launcher>
            COMMAND_EXPAND_LISTS)
    endif ()
endfunction()
