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
        src/EditorCamera.cpp
        src/NodeGraph/NodeGraph.cpp
        src/NodeGraph/NodeType.cpp
        src/NodeGraph/NodeGraphSerialize.cpp
        src/panels/AssetBrowserPanel.cpp
        src/panels/ConsolePanel.cpp
        src/panels/InspectorPanel.cpp
        src/panels/LevelEditorPanel.cpp
        src/panels/MaterialEditorPanel.cpp
        src/panels/PrefabEditorPanel.cpp
        src/panels/PrefabExplorerPanel.cpp
        src/panels/SceneViewportPanel.cpp
        src/panels/TextureEditorPanel.cpp
        src/FieldWidget.cpp
        src/material/MaterialPreview.cpp
        src/material/MaterialCatalog.cpp
        src/material/MaterialCompile.cpp
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
target_link_libraries(veng_editor PUBLIC veng::veng)

# Same -fno-exceptions enforcement as libveng: asserts are fatal, recoverable
# operations return Veng::Result<T>.
target_compile_options(veng_editor PRIVATE -fno-exceptions)
target_compile_definitions(veng_editor PRIVATE VE_BUILD_SHARED)

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
# never appears in a VengEditor/ public header). The imnodes symbols are hosted
# in veng::veng (its Vendor/ImGui.cpp aggregation TU compiles imnodes.cpp beside
# ImGui), so libveng_editor imports them across the PUBLIC veng::veng link; this
# PRIVATE link of imnodes' own target binds the symbols even if that aggregation
# changes, without adding a second imnodes.cpp build.
target_link_libraries(veng_editor PRIVATE imnodes)

# The editor launcher source veng_add_editor compiles per game (mirrors
# VENG_LAUNCHER_MAIN in Game.cmake), plus the cook-on-demand seam (CookSession).
# CookSession links libveng_cook — compiled into the exe, never into
# libveng_editor, so the importer dependency stays out of the framework library.
set(VENG_EDITOR_MAIN "${CMAKE_CURRENT_SOURCE_DIR}/src/editor_main.cpp"
    CACHE INTERNAL "veng editor launcher source")
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
# once here and copied beside every editor exe by veng_add_editor, mounted by
# EditorHost from ExecutableDirectory().
if (VENG_BUILD_TOOLS)
    add_asset_pack(veng_editor_icons
            PACK   assets/icons/editor_icons.vengpack.json
            OUTPUT ${CMAKE_BINARY_DIR}/editor_icons.vengpack)
    set(VENG_EDITOR_ICON_PACK "${CMAKE_BINARY_DIR}/editor_icons.vengpack"
        CACHE INTERNAL "veng editor icon pack (cooked)")
endif ()
