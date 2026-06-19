# Cook the sample asset pack with vengc at build time, into the build tree.
# veng_add_game copies the cooked pack beside the launcher; the app mounts it
# from ExecutableDirectory() so the launcher + module + pack are relocatable.
set(HT_ASSET_DIR "${CMAKE_CURRENT_BINARY_DIR}/assets")

add_asset_pack(hello_triangle_assets
        PACK   assets/sample.vengpack.json
        OUTPUT ${HT_ASSET_DIR}/sample.vengpack
        # The material's vertex shader references the core canonical vertex
        # layout by AssetId; resolve it from the engine core pack.
        REFERENCE ${CMAKE_SOURCE_DIR}/engine/assets/core/core.vengpack.json
        # The pack carries a prefab whose components include the game's Spinner,
        # so the cook reflects libhello_triangle's native types via --module.
        MODULE hello_triangle)

# The game splits into libhello_triangle (the runtime) and hello_triangle-launcher
# (the shipped exe that dlopens it). veng_add_game copies the cooked pack beside
# the launcher so the trio is a relocatable directory.
veng_add_game(hello_triangle
        SOURCES    main.cpp
        ASSET_PACK hello_triangle_assets)

# The game's editor module: the editor host dlopens it beside the game module to
# register the game's editor-only views. It registers none yet, exercising the
# editor-module load path. Links veng_editor for the EditorRegistry surface later
# plans register into.
add_library(hello_triangle_editor SHARED hello_triangle_editor.cpp)
target_link_libraries(hello_triangle_editor PRIVATE veng::veng veng_editor::veng_editor)

veng_add_editor(hello_triangle
        GAME_MODULE   hello_triangle
        EDITOR_MODULE hello_triangle_editor
        ASSET_PACK    hello_triangle_assets)
