# veng_add_editor(<name>
#     GAME_MODULE    <game target>             # libgame the editor loads (named in project.veng)
#     [EDITOR_MODULE <editor target>]          # optional libgame_editor, placed in the build dir
#     PROJECT        <add_project host target>  # the project whose project.veng the editor opens
# )
#
# Does NOT build an editor binary. The single shared veng-editor exe (built in editor/) is the
# editor for every project; this registers a `<name>-editor` *run target* that launches it with the
# project's source project.veng (--project). The module(s) are referenced by logical name from
# project.veng (its "module" / "editorModule" keys); the editor reads them and dlopens them from the
# project's build-output dir — the game ships only its library, not an editor shell.
#
# The build-output dir (where the module libraries + cooked packs land) is recorded beside the
# source project in a gitignored .veng/build.json sidecar, so the editor discovers it from the
# project path alone (no --build-dir needed). This is what lets a runtime project-picker launch the
# editor on a project with no CMake in the loop; --build-dir on veng-editor stays an override.
#
# The editor-extension module library is built by the caller
# (add_library(<name>_editor SHARED ...) linking veng_editor::veng_editor); this places it beside
# the launcher so the editor resolves it from the build dir, same as the game module.
#
# In-tree only: it serves the examples. veng-editor links libveng_cook (cook-on-demand) through
# CookSession; libveng_editor does not, so the importer table stays out of the framework library.

function(veng_add_editor NAME)
    cmake_parse_arguments(ARG "" "GAME_MODULE;EDITOR_MODULE;PROJECT" "" ${ARGN})

    if (NOT ARG_GAME_MODULE)
        message(FATAL_ERROR "veng_add_editor(${NAME}): GAME_MODULE is required")
    endif ()
    if (NOT ARG_PROJECT)
        message(FATAL_ERROR "veng_add_editor(${NAME}): PROJECT is required")
    endif ()
    if (NOT TARGET ${ARG_GAME_MODULE}-launcher)
        message(FATAL_ERROR
                "veng_add_editor(${NAME}): ${ARG_GAME_MODULE}-launcher must exist "
                "(call veng_add_game before veng_add_editor)")
    endif ()
    if (NOT TARGET veng-editor)
        message(FATAL_ERROR
                "veng_add_editor(${NAME}): the veng-editor shell is not built")
    endif ()

    # The module libraries + cooked packs live beside the launcher (veng_add_game's relocatable
    # set); the editor opens the project's source project.veng (baked absolute by add_project).
    set(BUILD_DIR $<TARGET_FILE_DIR:${ARG_GAME_MODULE}-launcher>)
    set(PROJECT_SOURCE $<TARGET_PROPERTY:${ARG_PROJECT},VENG_PROJECT_SOURCE>)
    set(EDITOR_DEPS veng-editor ${ARG_GAME_MODULE} ${ARG_GAME_MODULE}-launcher ${ARG_PROJECT})

    if (ARG_EDITOR_MODULE)
        # Place the editor module beside the launcher so the editor resolves it from the build dir.
        set_target_properties(${ARG_EDITOR_MODULE} PROPERTIES
                LIBRARY_OUTPUT_DIRECTORY ${BUILD_DIR})
        list(APPEND EDITOR_DEPS ${ARG_EDITOR_MODULE})
    endif ()

    # Record the build-output dir beside the source project, in a gitignored .veng/build.json
    # sidecar, so the editor (and a future project-picker launcher) discovers it from the project
    # path alone — no CMake in the launch loop. The source dir is a configure-time value; the
    # launcher's output dir is a generator expression, so file(GENERATE) writes it at generate time.
    # Last configure wins when a project is built in more than one tree.
    #
    # corePackManifest records the engine core pack's source manifest so the editor's cook-on-demand
    # resolves core-pack ids (the standard vertex shaders) — the same --reference the file-based
    # add_project cook passes; without it a recooked material referencing a core-pack shader fails.
    get_target_property(PROJECT_SRC_ABS ${ARG_PROJECT} VENG_PROJECT_SOURCE)
    get_filename_component(PROJECT_SRC_DIR "${PROJECT_SRC_ABS}" DIRECTORY)
    file(GENERATE OUTPUT "${PROJECT_SRC_DIR}/.veng/build.json"
            CONTENT "{\n  \"buildDir\": \"${BUILD_DIR}\",\n  \"corePackManifest\": \"${VENG_CORE_PACK_JSON}\"\n}\n")

    # Building the run target launches the editor against the project; the editor self-discovers the
    # build dir from the sidecar above (the same path the launcher will use). The dependencies build
    # the module(s), launcher, and the cooked project + packs first.
    add_custom_target(${NAME}-editor
            COMMAND $<TARGET_FILE:veng-editor> --project ${PROJECT_SOURCE}
            DEPENDS ${EDITOR_DEPS}
            USES_TERMINAL
            VERBATIM
            COMMENT "Launching veng-editor for ${NAME} (${PROJECT_SOURCE})")
endfunction()
