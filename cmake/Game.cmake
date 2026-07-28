# veng_add_game(<name>
#     SOURCES      <game sources...>     # the libgame translation units
#     [COOK_SOURCES <importer sources...>] # opt in: emit lib<name>_cook, the tool-only cook module
#     [PROJECT  <veng_add_project host target>] # optional: the project's host-config target
#     [MCP]                              # opt in: link the launcher against veng::mcp and
#                                        #   compile its --connect client short-circuit
# )
#
# COOK_SOURCES emits lib<name>_cook: a SHARED library exporting VengCookModuleRegister, which
# the cooker and the editor load beside lib<name> to obtain the game's own AssetImporters. It is
# a TOOL artifact: nothing at runtime loads it, so it is not one of the artifacts the relocatable
# set names (launcher + lib<name> + project + packs) and a ship copies those rather than the
# build directory. It links veng::cook_interface (the importer contract, headers only — never
# the static libveng_cook, which would carry a second copy of the cooker's process-wide state
# into the dlopened image), veng::veng, and lib<name> itself, so an importer shares the game's
# own format headers with the runtime loader that reads what it writes. It lands beside
# lib<name> so the sibling lookup (`lib<name>_cook` next to `--module`'s argument) resolves it.
# Name it to veng_add_project as COOK_MODULE <name>_cook so the cook is ordered behind it; that
# edge belongs on the cook command, which only veng_add_project declares.
#
# MCP turns the launcher into an MCP client: it links veng::mcp and compiles
# VENG_LAUNCHER_MCP into the exe, activating launcher_main.cpp's --connect
# short-circuit (a client of an already-running MCP server, driving one tool call and
# exiting before the game module loads). veng::mcp is an SDK library (exported in
# vengTargets, resolved by find_package(veng) like veng::graph), so the link resolves
# in all three consumption modes. Without MCP the launcher is byte-for-byte unchanged:
# no veng::mcp link, VENG_LAUNCHER_MCP undefined, the --connect code compiled out.
#
# Produces lib<name> (SHARED, links veng::veng) and <name>-launcher (exe, the veng
# launcher compiled with VENG_GAME_MODULE pointing at lib<name>).
#
# A PROJECT target (from veng_add_project) declares MODULE <name> so its cook reflects
# lib<name>'s native component types and the build graph orders lib<name> -> cook ->
# copy beside launcher. The cooked project file and every pack it names are copied
# beside the launcher (renamed to their un-suffixed mount names), so the relocatable
# set (launcher + lib + project + packs) is a self-contained, movable directory.
#
# The same body serves an in-tree build (veng as the top-level project) and a
# find_package(veng) consumer: only the launcher-source path differs, resolved per
# consumption mode below.

# VENG_LAUNCHER_MAIN is the launcher source compiled into each game's launcher. Its path
# is resolved per consumption mode: in-tree (VENG_PACKAGE_MODE unset) it is the source-tree
# file set here; a find_package(veng) consumer gets the installed copy, which veng-config
# sets before including this module.
if (NOT VENG_PACKAGE_MODE)
    set(VENG_LAUNCHER_MAIN "${CMAKE_CURRENT_LIST_DIR}/../engine/src/Launcher/launcher_main.cpp"
        CACHE INTERNAL "veng launcher source")
endif ()

function(veng_add_game NAME)
    cmake_parse_arguments(ARG "MCP" "PROJECT" "SOURCES;COOK_SOURCES" ${ARGN})

    add_library(${NAME} SHARED ${ARG_SOURCES})
    target_link_libraries(${NAME} PRIVATE veng::veng)

    # A game module is linked against, not only dlopen'd: its cook module, its editor module, and
    # its test binaries all resolve its symbols. macOS/Linux export them via default visibility, so
    # on MSVC auto-export to match — the same reason libveng itself does. /Yl- comes with it rather
    # than separately: auto-export folds the __@@_PchSym_@... marker MSVC injects into every
    # precompiled-header object into the generated exports.def as a data symbol named `__`, which
    # resolves to nothing and fails the module's own link.
    if (MSVC)
        set_target_properties(${NAME} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
        target_compile_options(${NAME} PRIVATE /Yl-)
    endif ()

    add_executable(${NAME}-launcher ${VENG_LAUNCHER_MAIN})
    target_link_libraries(${NAME}-launcher PRIVATE veng::veng)

    # MCP opt-in: the launcher becomes an MCP client (its --connect short-circuit),
    # nothing more. The game's server, if it has one, is started by the game module
    # (which links veng::mcp itself), not the launcher.
    if (ARG_MCP)
        target_link_libraries(${NAME}-launcher PRIVATE veng::mcp)
        target_compile_definitions(${NAME}-launcher PRIVATE VENG_LAUNCHER_MCP)
    endif ()
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

    # The optional cook module: tool-only, loaded by vengc and the editor, never shipped. It
    # lands beside lib<name> so the cooker's sibling lookup finds it with no extra flag, and it
    # is EXCLUDE_FROM_ALL-free on purpose — the cook needs it built, so the project's cook
    # targets depend on it below.
    if (ARG_COOK_SOURCES)
        add_library(${NAME}_cook SHARED ${ARG_COOK_SOURCES})
        target_link_libraries(${NAME}_cook PRIVATE veng::cook_interface veng::veng ${NAME})
        set_target_properties(${NAME}_cook PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:${NAME}-launcher>)

        # lib<name> resolves beside the cook module the same way it does beside the launcher, so
        # a tool that dlopens the cook module pulls the game lib in from the same directory.
        set_target_properties(${NAME}_cook PROPERTIES
            BUILD_RPATH   "${GAME_RPATH}"
            INSTALL_RPATH "${GAME_RPATH}")
    endif ()

    # Copy the cooked project file and every pack it names beside the launcher so
    # ExecutableDirectory()-relative loading finds them; the set (launcher + lib +
    # project + packs) is then a self-contained, movable directory. The host-config
    # project target records the cooked outputs (possibly suffixed per build
    # configuration) and the un-suffixed mount names the runtime loads, both set by
    # veng_add_project, so this copy reads them without the caller restating the paths. Each
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

        # Each copy stages through a sibling temporary and renames into place: the
        # rename is atomic, so a killed or concurrent build never leaves a torn
        # artifact with a fresh mtime that later builds would treat as up to date.
        set(PROJECT_BESIDE_LAUNCHER ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_MOUNT})
        add_custom_command(
            OUTPUT ${PROJECT_BESIDE_LAUNCHER}
            COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_OUTPUT} ${PROJECT_BESIDE_LAUNCHER}.tmp
            COMMAND ${CMAKE_COMMAND} -E rename ${PROJECT_BESIDE_LAUNCHER}.tmp ${PROJECT_BESIDE_LAUNCHER}
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
                COMMAND ${CMAKE_COMMAND} -E copy ${PACK_OUTPUT} ${PACK_BESIDE_LAUNCHER}.tmp
                COMMAND ${CMAKE_COMMAND} -E rename ${PACK_BESIDE_LAUNCHER}.tmp ${PACK_BESIDE_LAUNCHER}
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
