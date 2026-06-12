# add_shaders(<target> <shader sources...>)
#
# Compiles GLSL shaders with glslc into ${CMAKE_CURRENT_BINARY_DIR}/shaders/<name>.spv
# and wraps them in a custom target. Requires find_package(Vulkan) for Vulkan::glslc.
function(add_shaders TARGET_NAME)
    set(SHADER_SOURCE_FILES ${ARGN})

    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/shaders")

    set(SHADER_COMMAND)
    set(SHADER_PRODUCTS)

    foreach (SHADER_SOURCE IN LISTS SHADER_SOURCE_FILES)
        cmake_path(ABSOLUTE_PATH SHADER_SOURCE NORMALIZE)
        cmake_path(GET SHADER_SOURCE FILENAME SHADER_NAME)

        list(APPEND SHADER_COMMAND COMMAND)
        list(APPEND SHADER_COMMAND Vulkan::glslc)
        list(APPEND SHADER_COMMAND "${SHADER_SOURCE}")
        list(APPEND SHADER_COMMAND "-o")
        list(APPEND SHADER_COMMAND "${CMAKE_CURRENT_BINARY_DIR}/shaders/${SHADER_NAME}.spv")

        list(APPEND SHADER_PRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/shaders/${SHADER_NAME}.spv")
    endforeach ()

    add_custom_target(${TARGET_NAME} ALL
            COMMENT "Compiling Shaders [${TARGET_NAME}] ${SHADER_SOURCE_FILES}"
            SOURCES ${SHADER_SOURCE_FILES}
            BYPRODUCTS ${SHADER_PRODUCTS}
            ${SHADER_COMMAND}
    )
endfunction()
