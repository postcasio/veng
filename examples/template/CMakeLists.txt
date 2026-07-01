# Cook the template project with vengc at build time, into the build tree. veng_add_project
# reads project.veng for the packs and ship configurations and cooks each config's packs
# plus its cooked project file (.vengproj); veng_add_game copies the host-default config's
# project + packs beside the launcher (renamed to their un-suffixed mount names), so the
# launcher + module + project + pack are a relocatable directory.
set(TEMPLATE_ASSET_DIR "${CMAKE_CURRENT_BINARY_DIR}/assets")

# The project owns its pack + configuration set (project.veng lists assets/template.vengpack.json
# and the three ship targets with macOS active). VENG_BUILD_CONFIG (defaulting from the host
# triple) names the configuration the launcher's artifacts cook under.
veng_add_project(template_assets
        PROJECT    project.veng
        OUTPUT_DIR ${TEMPLATE_ASSET_DIR}
        # The pack carries a prefab + level, so the cook reflects libtemplate's types and
        # systems via --module (here only the engine builtins).
        MODULE     template
        # The material's vertex shader references the core canonical vertex layout by
        # AssetId; resolve it from the engine core pack.
        REFERENCE  ${CMAKE_SOURCE_DIR}/engine/assets/core/core.vengpack.json)

# The game splits into libtemplate (the runtime) and template-launcher (the shipped exe that
# dlopens it). veng_add_game copies the host-default config's cooked project + packs beside the
# launcher so the set is a relocatable directory.
veng_add_game(template
        SOURCES main.cpp
        PROJECT ${template_assets_HOST_TARGET})

# The minimal app gets editor support for free: the shared veng-editor opens its project, loads
# libtemplate (named in project.veng), and edits its live sources. `cmake --build build --target
# template-editor` launches it. No editor-extension module — the template registers no custom panels.
veng_add_editor(template
        GAME_MODULE template
        PROJECT     ${template_assets_HOST_TARGET})
