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
    if (ARG_ASSET_PACK)
        add_dependencies(${NAME}-launcher ${ARG_ASSET_PACK})
        add_custom_command(TARGET ${NAME}-launcher POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_PROPERTY:${ARG_ASSET_PACK},VENG_ASSET_PACK_OUTPUT>
                $<TARGET_FILE_DIR:${NAME}-launcher>)
    endif ()
endfunction()
