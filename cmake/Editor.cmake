# veng_add_editor(<name>
#     GAME_MODULE    <game target>        # libgame to dlopen at runtime
#     [EDITOR_MODULE <editor target>]     # optional libgame_editor
#     [PROJECT       <add_project host target>] # the project whose project.veng the
#                                         # editor opens (reads its packs to resolve an
#                                         # AssetId to its per-asset JSON source)
# )
#
# Produces lib<name>_editor (SHARED, links veng_editor::veng_editor) and
# <name>-editor (exe, links veng::veng + veng_editor::veng_editor). The editor exe
# is placed beside the game launcher binary so both resolve the game module via
# @loader_path / $ORIGIN; the cooked pack copied beside the launcher serves it too.
#
# VENG_EDITOR_GAME_MODULE / VENG_EDITOR_EDITOR_MODULE are baked into the exe as the
# module file names it dlopen's (the editor-side analogue of VENG_GAME_MODULE).
#
# In-tree only: it serves the example. The editor exe links libveng_cook
# (cook-on-demand) PRIVATE; libveng_editor does not, so the importer table stays
# out of the framework library and never reaches libveng or libgame.

function(veng_add_editor NAME)
    cmake_parse_arguments(ARG "" "GAME_MODULE;EDITOR_MODULE;PROJECT" "" ${ARGN})

    if (NOT ARG_GAME_MODULE)
        message(FATAL_ERROR "veng_add_editor(${NAME}): GAME_MODULE is required")
    endif ()

    add_executable(${NAME}-editor ${VENG_EDITOR_MAIN} ${VENG_EDITOR_COOK_SESSION})
    target_link_libraries(${NAME}-editor PRIVATE
            veng::veng veng_editor::veng_editor veng::cook)
    target_include_directories(${NAME}-editor PRIVATE ${VENG_EDITOR_EXE_INCLUDE})

    target_compile_definitions(${NAME}-editor PRIVATE
            VENG_EDITOR_GAME_MODULE="$<TARGET_FILE_NAME:${ARG_GAME_MODULE}>")

    # Resolve the dlopen'd modules beside the editor binary (same rationale as the
    # launcher's rpath in Game.cmake).
    if (APPLE)
        set(EDITOR_RPATH "@loader_path")
    else ()
        set(EDITOR_RPATH "$ORIGIN")
    endif ()
    set_target_properties(${NAME}-editor PROPERTIES
            BUILD_RPATH   "${EDITOR_RPATH}"
            INSTALL_RPATH "${EDITOR_RPATH}")

    add_dependencies(${NAME}-editor ${ARG_GAME_MODULE})

    # Place the editor exe in the same directory as the game launcher so it shares
    # the game module lib and the cooked pack already copied there by veng_add_game.
    if (TARGET ${ARG_GAME_MODULE}-launcher)
        set_target_properties(${NAME}-editor PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:${ARG_GAME_MODULE}-launcher>)
        add_dependencies(${NAME}-editor ${ARG_GAME_MODULE}-launcher)
    endif ()

    # The editor opens the project's project.veng: it reads the packs the project owns to
    # map an AssetId to its per-asset JSON source (so the texture editor knows which
    # .tex.json to edit and recook) and loads the build configurations. The path is the
    # in-tree source, baked absolute — the editor is in-tree only, editing live sources.
    if (ARG_PROJECT)
        target_compile_definitions(${NAME}-editor PRIVATE
                VENG_EDITOR_PROJECT="$<TARGET_PROPERTY:${ARG_PROJECT},VENG_PROJECT_SOURCE>")
        add_dependencies(${NAME}-editor ${ARG_PROJECT})
    endif ()

    # Copy the cooked editor icon pack beside the editor exe so EditorHost mounts it
    # from ExecutableDirectory() (the same relocatable-trio rule the game pack follows).
    # The engine ships no icon content; these are the editor's own light/camera billboards.
    if (TARGET veng_editor_icons)
        add_dependencies(${NAME}-editor veng_editor_icons)
        add_custom_command(TARGET ${NAME}-editor POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${VENG_EDITOR_ICON_PACK} $<TARGET_FILE_DIR:${NAME}-editor>
                COMMENT "Copying editor icon pack beside ${NAME}-editor")
    endif ()

    if (ARG_EDITOR_MODULE)
        target_compile_definitions(${NAME}-editor PRIVATE
                VENG_EDITOR_EDITOR_MODULE="$<TARGET_FILE_NAME:${ARG_EDITOR_MODULE}>")
        add_dependencies(${NAME}-editor ${ARG_EDITOR_MODULE})

        # Place the editor module beside the editor exe so @loader_path/$ORIGIN
        # finds it.
        set_target_properties(${ARG_EDITOR_MODULE} PROPERTIES
                LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:${NAME}-editor>)
    endif ()

    # Windows has no rpath: copy the editor's dependent DLLs (libveng, libveng_editor,
    # slang.dll via veng::cook) beside the exe so it runs standalone, mirroring the
    # @loader_path/$ORIGIN resolution above.
    if (WIN32)
        add_custom_command(TARGET ${NAME}-editor POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${NAME}-editor> $<TARGET_FILE_DIR:${NAME}-editor>
            COMMAND_EXPAND_LISTS)
    endif ()
endfunction()
