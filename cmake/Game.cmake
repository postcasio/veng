# veng_add_game(<name>
#     SOURCES    <game sources...>       # the libgame translation units
#     [ASSET_PACK <pack target>]         # optional: an add_asset_pack target to depend on
# )
#
# Produces lib<name> (SHARED, links veng::veng) and <name>-launcher (exe, the veng
# launcher compiled with VENG_GAME_MODULE pointing at lib<name>). The EDITOR arm
# (lib<name>_editor) is reserved for the editor planset — libveng_editor does not
# exist yet — and is intentionally not emitted here.
#
# A prefab-bearing ASSET_PACK declares add_asset_pack(... MODULE <name>) so its
# cook reflects lib<name>'s native component types and the build graph orders
# lib<name> -> cook -> copy beside launcher. The relocatable trio is preserved.
#
# In-tree only this planset: it serves the example and the tests, which build veng
# as the top-level project. Installed-package wiring (resolving the launcher source
# from an installed veng) is a separate packaging concern, out of scope here.

# VENG_LAUNCHER_MAIN is the launcher source. In-tree it is the source-tree path,
# set right here.
set(VENG_LAUNCHER_MAIN "${CMAKE_CURRENT_LIST_DIR}/../engine/src/Launcher/launcher_main.cpp"
    CACHE INTERNAL "veng launcher source")

function(veng_add_game NAME)
    cmake_parse_arguments(ARG "" "ASSET_PACK" "SOURCES" ${ARGN})

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

    # Copy the cooked pack beside the launcher so ExecutableDirectory()-relative
    # mounting finds it; the trio (launcher + lib + pack) is then a self-contained,
    # movable directory. The pack target records its cooked-file path as the
    # VENG_ASSET_PACK_OUTPUT property (set by add_asset_pack) so this copy reads it
    # without the caller restating the path.
    #
    # The copy is its own output-producing command keyed on the cooked pack, not a
    # POST_BUILD step on the launcher: a re-cook (e.g. a prefab edit) does not relink
    # the launcher, so a POST_BUILD copy would never fire and the launcher would mount
    # a stale pack. Depending on the cooked file makes the copy re-run whenever the
    # pack changes; depending on the launcher target gives the destination directory
    # and a stable build order without a cycle (the launcher does not depend back).
    if (ARG_ASSET_PACK)
        add_dependencies(${NAME}-launcher ${ARG_ASSET_PACK})

        # The launcher sets no custom RUNTIME_OUTPUT_DIRECTORY, so it lands in this
        # directory's binary dir; the pack copy targets the same dir. A target-dependent
        # genex is not permitted in a custom command OUTPUT, so the destination is a
        # plain configure-time path.
        get_target_property(PACK_OUTPUT ${ARG_ASSET_PACK} VENG_ASSET_PACK_OUTPUT)
        cmake_path(GET PACK_OUTPUT FILENAME PACK_FILENAME)
        set(PACK_BESIDE_LAUNCHER ${CMAKE_CURRENT_BINARY_DIR}/${PACK_FILENAME})

        add_custom_command(
            OUTPUT ${PACK_BESIDE_LAUNCHER}
            COMMAND ${CMAKE_COMMAND} -E copy ${PACK_OUTPUT} ${PACK_BESIDE_LAUNCHER}
            DEPENDS ${PACK_OUTPUT} ${NAME}-launcher
            COMMENT "Copying asset pack beside ${NAME}-launcher")
        add_custom_target(${NAME}-launcher-pack ALL DEPENDS ${PACK_BESIDE_LAUNCHER})
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
