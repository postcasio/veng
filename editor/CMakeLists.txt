# ---------------------------------------------------------------------------
# libveng_editor — the editor framework library
# ---------------------------------------------------------------------------
#
# The editor host, panels, EditorRegistry, and dockspace. Links veng::veng
# PRIVATE (the engine runtime, behind the editor's own surface) and the ImGui
# headers PRIVATE (the editor draws its UI directly). libveng_cook is NOT linked
# here; cook-on-demand is a later plan.

add_library(veng_editor SHARED
        src/EditorHost.cpp
        src/EditorPanel.cpp
        src/AssetEditorPanel.cpp
        src/AssetSourceIndex.cpp
        src/AssetChip.cpp
        src/CommandStack.cpp
        src/EditorCommand.cpp
        src/EditorCamera.cpp
        src/EditorGizmo.cpp
        src/PreviewCapability.cpp
        src/PrefabSerialize.cpp
        src/StatusTracker.cpp
        src/panels/AssetBrowserPanel.cpp
        src/panels/ConsolePanel.cpp
        src/panels/InspectorPanel.cpp
        src/panels/LevelEditorPanel.cpp
        src/panels/MaterialEditorPanel.cpp
        src/panels/MaterialInstanceEditorPanel.cpp
        src/panels/PrefabEditorPanel.cpp
        src/panels/PrefabExplorerPanel.cpp
        src/panels/ProjectSettingsPanel.cpp
        src/panels/SceneViewportPanel.cpp
        src/panels/TextureEditorPanel.cpp
        src/FieldWidget.cpp
        src/FieldWidgetDispatch.cpp
        src/FieldGate.cpp
        src/material/MaterialPreview.cpp
)
add_library(veng_editor::veng_editor ALIAS veng_editor)

target_include_directories(veng_editor PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# src-private headers (the material specialization) address each other by their
# editor/src-relative path (material/MaterialCompile.h); the test targets put the
# same dir on their include path so cases reach these headers.
target_include_directories(veng_editor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# The editor's public headers pull in ImGui (a UI library, part of the editor's
# surface) but never Vulkan/GLFW — same boundary as libveng. veng::veng already
# propagates the ImGui include dirs and hosts the ImGui symbols; link it PUBLIC so
# an editor module compiling against VengEditor headers resolves <imgui.h>.
# veng::graph is the shared node-graph + material-codegen library the editor's
# node-graph UI and material editor build on; PUBLIC so an editor module reaches
# its VengGraph/ headers.
target_link_libraries(veng_editor PUBLIC veng::veng veng::graph)

# Same -fno-exceptions enforcement as libveng: asserts are fatal, recoverable
# operations return Veng::Result<T>.
target_compile_options(veng_editor PRIVATE -fno-exceptions)
target_compile_definitions(veng_editor PRIVATE VE_BUILD_SHARED)

# Precompile the foundational Veng.h here too (most editor TUs include it). Its own
# PCH rather than REUSE_PRECOMPILE_HEADER_FROM veng: the predefine sets differ
# (veng's private IMGUI_USER_CONFIG / tinyexr macros), which would reject a shared PCH.
target_precompile_headers(veng_editor PRIVATE <Veng/Veng.h>)

# Auto-export all symbols on MSVC so the editor exe links against this shared library
# without the VE_API annotation being complete — mirrors macOS/Linux default visibility.
if (MSVC)
    set_target_properties(veng_editor PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif ()

# nlohmann/json — the texture editor reads/writes per-asset .tex.json sources and
# the asset source index parses the pack manifest. Used only in .cpp (never in a
# public header), so it is a PRIVATE dependency: it does not leak into editor
# modules or libveng. JSON_NOEXCEPTION matches the cooker's contract under
# -fno-exceptions (JSON_THROW becomes std::abort, guarded by is_*() checks).
target_link_libraries(veng_editor PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(veng_editor PRIVATE JSON_NOEXCEPTION)

# imnodes — the material editor's node canvas, included only in .cpp (imnodes.h
# never appears in a VengEditor/ public header). imnodes.cpp is compiled exactly
# once, into veng::veng's Vendor/ImGui.cpp aggregation TU beside ImGui, and
# libveng_editor imports those symbols across the PUBLIC veng::veng link (the
# imnodes.h include dir rides veng's PUBLIC interface). Do NOT link imnodes' own
# static target here: that compiles a second imnodes.cpp into libveng_editor,
# giving it a private GImNodes the engine's ImNodes::CreateContext (in libveng)
# never initializes — under two-level namespaces the editor's ImNodes calls bind
# to that null context and crash on first use.

# The editor shell's main and the cook-on-demand seam (CookSession). CookSession
# links libveng_cook — compiled into the exe, never into libveng_editor, so the
# importer dependency stays out of the framework library.
set(VENG_EDITOR_MAIN "${CMAKE_CURRENT_SOURCE_DIR}/src/editor_main.cpp"
    CACHE INTERNAL "veng editor shell main source")
set(VENG_EDITOR_COOK_SESSION "${CMAKE_CURRENT_SOURCE_DIR}/src/CookSession.cpp"
    CACHE INTERNAL "veng editor cook-on-demand source")
set(VENG_EDITOR_EXE_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    CACHE INTERNAL "veng editor exe-private include dir (CookSession.h)")

# ---------------------------------------------------------------------------
# Editor icon pack — light/camera billboard icons for the scene viewport gizmos
# ---------------------------------------------------------------------------
#
# The engine ships no icon content; the editor cooks its own icon textures and
# resolves them to bindless TextureHandles for the debug-draw billboards. Cooked
# once here and copied beside the editor exe (its own content, mounted by EditorHost
# from ExecutableDirectory() — distinct from a project's build dir).
#
# The editor is the heavy GUI surface above the cooker, built top-level-only (the cooker
# beneath it always builds from source).
if (PROJECT_IS_TOP_LEVEL)
    add_asset_pack(veng_editor_icons
            PACK   assets/icons/editor_icons.vengpack.json
            OUTPUT ${CMAKE_BINARY_DIR}/editor_icons.vengpack)
    set(VENG_EDITOR_ICON_PACK "${CMAKE_BINARY_DIR}/editor_icons.vengpack"
        CACHE INTERNAL "veng editor icon pack (cooked)")
endif ()

# ---------------------------------------------------------------------------
# veng-editor — the single, project-agnostic editor shell
# ---------------------------------------------------------------------------
#
# One editor exe for every project (no per-game editor binary). Launched with
# `--project <project.veng> --build-dir <build output>`; it reads the module(s) the
# project names and dlopens them from the build dir. veng_add_editor (cmake/Editor.cmake)
# registers a per-project `<name>-editor` run target that launches this with the right
# arguments. Links libveng_cook PRIVATE through CookSession (the importer never reaches
# libveng_editor). In-build it resolves libveng/libveng_editor/slang via the auto build rpath.
if (PROJECT_IS_TOP_LEVEL)
    add_executable(veng-editor ${VENG_EDITOR_MAIN} ${VENG_EDITOR_COOK_SESSION})
    target_link_libraries(veng-editor PRIVATE veng::veng veng_editor::veng_editor veng::cook)
    target_include_directories(veng-editor PRIVATE ${VENG_EDITOR_EXE_INCLUDE})

    # The editor's own icon pack sits beside the exe (its own content, ExecutableDirectory()-mounted).
    if (TARGET veng_editor_icons)
        add_dependencies(veng-editor veng_editor_icons)
        add_custom_command(TARGET veng-editor POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${VENG_EDITOR_ICON_PACK} $<TARGET_FILE_DIR:veng-editor>
                COMMENT "Copying editor icon pack beside veng-editor")
    endif ()

    # Windows has no rpath: copy the editor's dependent DLLs (libveng, libveng_editor, slang.dll
    # via veng::cook) beside the exe so it runs standalone.
    if (WIN32)
        add_custom_command(TARGET veng-editor POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:veng-editor> $<TARGET_FILE_DIR:veng-editor>
            COMMAND_EXPAND_LISTS)
    endif ()
endif ()
