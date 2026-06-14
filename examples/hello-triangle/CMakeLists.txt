add_executable(hello_triangle main.cpp)

target_link_libraries(hello_triangle PRIVATE veng::veng)

# Shaders are loaded at runtime from the build tree; bake the path in so the
# sample can be run from any working directory.
target_compile_definitions(hello_triangle PRIVATE
        HT_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders"
)

add_shaders(hello_triangle_shaders
        shaders/triangle.vert
        shaders/triangle.frag
        shaders/composite.vert
        shaders/composite.frag
)
add_dependencies(hello_triangle hello_triangle_shaders)

# Cook the sample asset pack (plan 06) with vengc at build time, into the
# build tree alongside the shaders.
set(HT_ASSET_DIR "${CMAKE_CURRENT_BINARY_DIR}/assets")

add_custom_command(
        OUTPUT ${HT_ASSET_DIR}/sample.vengpack
        COMMAND ${CMAKE_COMMAND} -E make_directory ${HT_ASSET_DIR}
        COMMAND $<TARGET_FILE:vengc> cook
                ${CMAKE_CURRENT_SOURCE_DIR}/assets/sample.vengpack.json
                -o ${HT_ASSET_DIR}/sample.vengpack
        DEPENDS vengc
                ${CMAKE_CURRENT_SOURCE_DIR}/assets/sample.vengpack.json
                ${CMAKE_CURRENT_SOURCE_DIR}/assets/textures/brick.tex.json
                ${CMAKE_CURRENT_SOURCE_DIR}/assets/textures/brick.png
        COMMENT "Cooking hello-triangle asset pack"
)

add_custom_target(hello_triangle_assets DEPENDS ${HT_ASSET_DIR}/sample.vengpack)
add_dependencies(hello_triangle hello_triangle_assets)

target_compile_definitions(hello_triangle PRIVATE
        HT_ASSET_DIR="${HT_ASSET_DIR}"
)
