# Cook the hello-triangle project with vengc at build time, into the build tree. veng_add_project
# reads project.veng for the packs and ship configurations and cooks each config's packs plus
# its cooked project file (.vengproj); veng_add_game copies the host-default config's project +
# packs beside the launcher (renamed to their un-suffixed mount names), so the launcher + module
# + project + pack are a relocatable directory.
set(HT_ASSET_DIR "${CMAKE_CURRENT_BINARY_DIR}/assets")

# The project owns its pack + configuration set (project.veng lists assets/sample.vengpack.json
# and the three ship targets with macOS active). VENG_BUILD_CONFIG (defaulting from the host
# triple) names the configuration the launcher's artifacts cook under.
veng_add_project(hello_triangle_assets
        PROJECT    project.veng
        OUTPUT_DIR ${HT_ASSET_DIR}
        # The pack carries a prefab whose components include the game's Spinner, so the cook
        # reflects libhello_triangle's native types via --module.
        MODULE     hello_triangle
        # The material's vertex shader references the core canonical vertex layout by AssetId;
        # resolve it from the engine core pack.
        REFERENCE  ${CMAKE_SOURCE_DIR}/engine/assets/core/core.vengpack.json)

# The game splits into libhello_triangle (the runtime) and hello_triangle-launcher (the shipped
# exe that dlopens it). veng_add_game copies the host-default config's cooked project + packs
# beside the launcher so the set is a relocatable directory.
veng_add_game(hello_triangle
        SOURCES main.cpp
        PROJECT ${hello_triangle_assets_HOST_TARGET})

# The maximal sample links veng::mcp — the optional MCP server library — as the worked example
# a consumer copies: construct an McpServer from an McpHost, pump it once per frame. It stays
# env-gated behind HT_MCP in main.cpp, so the default HT_SMOKE/golden path opens no socket. Only
# the JSON-library-free Veng/Mcp/ public headers are used, so no nlohmann link is needed here.
target_link_libraries(hello_triangle PRIVATE veng::mcp)

# The game's editor module: the editor host dlopens it beside the game module to register the
# game's editor-only views. It registers none yet, exercising the editor-module load path.
add_library(hello_triangle_editor SHARED hello_triangle_editor.cpp)
target_link_libraries(hello_triangle_editor PRIVATE veng::veng veng_editor::veng_editor)

veng_add_editor(hello_triangle
        GAME_MODULE   hello_triangle
        EDITOR_MODULE hello_triangle_editor
        PROJECT       ${hello_triangle_assets_HOST_TARGET}
        # Launch the editor with its MCP server on a fixed loopback port, mutations enabled,
        # so an agent can inspect and edit the scene.
        ARGS          --mcp=5123 --mcp-write)

# Convenience run target: launch the game with its MCP server on the same fixed port (HT_MCP=<port>),
# mutations enabled (HT_MCP_WRITE). The smoke/golden ctests set their own environment, so this leaves
# them untouched (no socket).
add_custom_target(hello_triangle-run
        COMMAND ${CMAKE_COMMAND} -E env HT_MCP=5123 HT_MCP_WRITE=1 $<TARGET_FILE:hello_triangle-launcher>
        DEPENDS hello_triangle-launcher
        USES_TERMINAL
        VERBATIM
        COMMENT "Launching hello_triangle with MCP on 127.0.0.1:5123 (writes enabled)")
