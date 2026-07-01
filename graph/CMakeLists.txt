# ---------------------------------------------------------------------------
# libveng_graph — the shared node-graph + material-codegen library
# ---------------------------------------------------------------------------
#
# The generic node-graph topology core (NodeGraph/NodeType/NodeGraphSerialize), the
# material node catalog, and the topological emit walk (CompileMaterialGraph) that
# turns a material graph into generated Slang source. Linked PUBLIC by both
# libveng_editor (which draws the graph UI) and libveng_cook (which generates the
# fragment shader from a graph), so the editor preview and the offline cook run the
# identical walk. Knows nothing of ImGui or Vulkan — it names only Veng reflection,
# string/PinType/glm in its public headers, and nlohmann/json privately in its .cpp.

add_library(veng_graph SHARED
        src/NodeGraph.cpp
        src/NodeType.cpp
        src/NodeGraphSerialize.cpp
        src/MaterialCatalog.cpp
        src/MaterialMathNodes.cpp
        src/MaterialCompile.cpp
)
add_library(veng::graph ALIAS veng_graph)

target_include_directories(veng_graph PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# veng::veng — the public headers name Veng reflection, AssetHandle/Material, and the
# house vocabulary (string/vector/glm). PUBLIC so a consumer (editor, cooker)
# resolves those includes through this link.
target_link_libraries(veng_graph PUBLIC veng::veng)

# Same -fno-exceptions enforcement as libveng: asserts are fatal, recoverable
# operations return Veng::Result<T>.
target_compile_options(veng_graph PRIVATE -fno-exceptions)
target_compile_definitions(veng_graph PRIVATE VE_BUILD_SHARED)

target_precompile_headers(veng_graph PRIVATE <Veng/Veng.h>)

# Auto-export all symbols on MSVC so consumers link against this shared library
# without complete VE_API annotation — mirrors macOS/Linux default visibility.
if (MSVC)
    set_target_properties(veng_graph PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif ()

# nlohmann/json — graph (de)serialization and the .vmat field-list write use it only
# in .cpp (never a public header), so it is a PRIVATE dependency: it leaks into
# neither consumers nor libveng. JSON_NOEXCEPTION matches the -fno-exceptions
# contract (JSON_THROW becomes std::abort, guarded by is_*() checks).
target_link_libraries(veng_graph PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(veng_graph PRIVATE JSON_NOEXCEPTION)

# veng_graph is a shared library the installed vengc loads at runtime (vengc →
# veng::cook → veng::graph), so it joins vengTargets and installs beside libveng —
# the installed cooker resolves it through @loader_path/../lib. Its public headers
# and downstream ALIAS are installed in a later plan; this install makes the imported
# vengc runnable from the prefix.
install(TARGETS veng_graph
        EXPORT vengTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
