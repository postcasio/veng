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
        DEPENDS
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/textures/brick.tex.json
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/textures/brick.png
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/meshes/cube.mesh.json
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/meshes/cube.obj
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/material_data.slang
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/brick.vert.slang
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/brick.frag.slang
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/brick.vert.shader.json
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/brick.frag.shader.json
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/composite.vert.slang
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/composite.frag.slang
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/composite.vert.shader.json
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/composite.frag.shader.json
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/materials/brick.vmat.json)

# The game splits into libhello_triangle (the runtime) and hello_triangle-launcher
# (the shipped exe that dlopens it). veng_add_game copies the cooked pack beside
# the launcher so the trio is a relocatable directory.
veng_add_game(hello_triangle
        SOURCES    main.cpp
        ASSET_PACK hello_triangle_assets)
