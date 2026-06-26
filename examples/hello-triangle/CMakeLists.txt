# Cook the sample asset pack with vengc at build time, into the build tree.
# veng_add_game copies the host-default config's cooked pack beside the launcher
# (renamed to the un-suffixed mount name); the app mounts it from
# ExecutableDirectory() so the launcher + module + pack are relocatable.
set(HT_ASSET_DIR "${CMAKE_CURRENT_BINARY_DIR}/assets")

# The project owns its configuration set (configs/project.veng lists both with macOS
# active) and the host -> config map: a name -> buildcfg lookup, since the engine owns
# only the selection variable and host-triple default. VENG_BUILD_CONFIG (defaulting
# from the host triple) names the configuration the launcher's pack cooks under.
set(HT_CONFIG_macos   ${CMAKE_CURRENT_SOURCE_DIR}/configs/macos.buildcfg)
set(HT_CONFIG_windows ${CMAKE_CURRENT_SOURCE_DIR}/configs/windows.buildcfg)
set(HT_SHIP_CONFIGS macos windows)

if (NOT DEFINED HT_CONFIG_${VENG_BUILD_CONFIG})
    message(FATAL_ERROR
            "hello-triangle: no build configuration '${VENG_BUILD_CONFIG}' (have: ${HT_SHIP_CONFIGS})")
endif ()

# Cook every ship configuration into its own per-config pack target so cook-all-packs
# can build them all; the launcher copy below wires only the host-default pack.
foreach (CFG IN LISTS HT_SHIP_CONFIGS)
    add_asset_pack(hello_triangle_assets
            PACK   assets/sample.vengpack.json
            OUTPUT ${HT_ASSET_DIR}/sample.vengpack
            CONFIG ${HT_CONFIG_${CFG}}
            # The material's vertex shader references the core canonical vertex
            # layout by AssetId; resolve it from the engine core pack.
            REFERENCE ${CMAKE_SOURCE_DIR}/engine/assets/core/core.vengpack.json
            # The pack carries a prefab whose components include the game's Spinner,
            # so the cook reflects libhello_triangle's native types via --module.
            MODULE hello_triangle)
    veng_register_all_packs_target(${hello_triangle_assets_TARGET})
    if (CFG STREQUAL VENG_BUILD_CONFIG)
        set(HT_HOST_PACK_TARGET ${hello_triangle_assets_TARGET})
    endif ()
endforeach ()

# The game splits into libhello_triangle (the runtime) and hello_triangle-launcher
# (the shipped exe that dlopens it). veng_add_game copies the host-default config's
# cooked pack beside the launcher so the trio is a relocatable directory.
veng_add_game(hello_triangle
        SOURCES    main.cpp
        ASSET_PACK ${HT_HOST_PACK_TARGET})

# The game's editor module: the editor host dlopens it beside the game module to
# register the game's editor-only views. It registers none yet, exercising the
# editor-module load path. Links veng_editor for the EditorRegistry surface later
# plans register into.
add_library(hello_triangle_editor SHARED hello_triangle_editor.cpp)
target_link_libraries(hello_triangle_editor PRIVATE veng::veng veng_editor::veng_editor)

veng_add_editor(hello_triangle
        GAME_MODULE   hello_triangle
        EDITOR_MODULE hello_triangle_editor
        ASSET_PACK    ${HT_HOST_PACK_TARGET})
