# veng_add_editor(<name>
#     GAME_MODULE    <game target>        # libgame to dlopen at runtime
#     [EDITOR_MODULE <editor target>]     # optional libgame_editor
#     [ASSET_PACK    <pack target>]       # an add_asset_pack target whose source
#                                         # manifest the editor reads to resolve an
#                                         # AssetId to its per-asset JSON source
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
    cmake_parse_arguments(ARG "" "GAME_MODULE;EDITOR_MODULE;ASSET_PACK" "" ${ARGN})

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

    # The editor reads the pack's source manifest to map an AssetId to its
    # per-asset JSON source (so the texture editor knows which .tex.json to edit
    # and recook). The path is the in-tree source manifest, baked absolute — the
    # editor is in-tree only, editing the live source files.
    if (ARG_ASSET_PACK)
        target_compile_definitions(${NAME}-editor PRIVATE
                VENG_EDITOR_ASSET_MANIFEST="$<TARGET_PROPERTY:${ARG_ASSET_PACK},VENG_ASSET_PACK_SOURCE>")
        add_dependencies(${NAME}-editor ${ARG_ASSET_PACK})
    endif ()

    # Bake the fonts directory path into the editor exe so it can resolve the
    # Roboto font at runtime. Like VENG_EDITOR_ASSET_MANIFEST, this is absolute
    # and in-tree — the editor is a development tool, not a shipped binary.
    target_compile_definitions(${NAME}-editor PRIVATE
            VENG_EDITOR_FONTS_DIR="${CMAKE_SOURCE_DIR}/editor/fonts")

    if (ARG_EDITOR_MODULE)
        target_compile_definitions(${NAME}-editor PRIVATE
                VENG_EDITOR_EDITOR_MODULE="$<TARGET_FILE_NAME:${ARG_EDITOR_MODULE}>")
        add_dependencies(${NAME}-editor ${ARG_EDITOR_MODULE})

        # Place the editor module beside the editor exe so @loader_path/$ORIGIN
        # finds it.
        set_target_properties(${ARG_EDITOR_MODULE} PROPERTIES
                LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:${NAME}-editor>)
    endif ()
endfunction()
