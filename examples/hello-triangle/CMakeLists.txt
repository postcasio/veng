add_executable(hello_triangle main.cpp)

target_link_libraries(hello_triangle PRIVATE veng::veng)

# Cook the sample asset pack with vengc at build time, into the build tree. The
# sample bakes the directory in via a compile definition so it can be run from
# any working directory.
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
add_dependencies(hello_triangle hello_triangle_assets)

target_compile_definitions(hello_triangle PRIVATE
        HT_ASSET_DIR="${HT_ASSET_DIR}"
)
