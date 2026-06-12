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
