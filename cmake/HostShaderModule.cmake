# veng_add_host_shader_module(<target>
#     MODULE <file.slang>        # the host exports file, relative to CMAKE_CURRENT_SOURCE_DIR
#     NAME   <Name>              # C++ namespace, and the generated <Name>.h / <Name>.cpp
#     [SHADER_INCLUDE <dir>]     # engine core shader dir on the search path (default: veng's)
#     [DEPENDS <files...>]       # extra regeneration triggers outside the depfile's view
# )
#
# Compiles a pure Slang module for the host with `vengc compile-host` and wraps the generated C++
# in a STATIC library <target>. The generated header's directory is a PUBLIC include, so a
# consumer writes `#include <Name.h>` and calls `Name::Fn(...)` with glm types; the library links
# glm and nothing else, so a process using it carries no Slang and no Vulkan at run time. vengc
# writes a depfile naming every file the module pulled in, so editing an included file
# regenerates the library.
#
# The generated unit is the compiler's text, not veng's, so it is held to a different bar:
# - C++20, since the Slang prelude includes <stdfloat> under C++23 and later, which Apple's libc++
#   lacks;
# - warnings never errors (-Wno-error / /WX-), and no clang-tidy (SKIP_LINTING);
# - no fast math and no floating-point contraction (-ffp-contract=off, /fp:precise), so an arm64
#   host and an x64 host evaluate the same expressions the same way.
#
# The host and the GPU agree to rounding, not to the bit. Measured on the reference fixture
# (gradient noise and a 16-octave fractal sum, |p| <= 256, Slang 2026.18.3, MoltenVK with fast
# math): host float against GPU float within 9e-7 on the value and 5.3e-6 on the gradient. The
# guide (docs/guides/shader-code-on-the-host.md) states the bound a consumer may rely on.
#
# It works in every consumption mode (in-tree, build tree, install), needing only the imported
# vengc.

# The Slang the host compile was validated against. The emitted C++ is the installed compiler's
# and Slang is not vendored, so a different version is reported once at configure time.
set(VENG_HOST_SHADER_SLANG_VERSION "2026.18.3")

function(_veng_check_host_shader_slang_version)
    get_property(checked GLOBAL PROPERTY VENG_HOST_SHADER_SLANG_CHECKED)
    if (checked)
        return()
    endif ()
    set_property(GLOBAL PROPERTY VENG_HOST_SHADER_SLANG_CHECKED TRUE)
    find_path(VENG_SLANG_TAG_HEADER_DIR slang/slang-tag-version.h)
    if (NOT VENG_SLANG_TAG_HEADER_DIR)
        message(WARNING "veng_add_host_shader_module: slang/slang-tag-version.h not found, so the "
                "Slang version cannot be checked against ${VENG_HOST_SHADER_SLANG_VERSION}")
        return()
    endif ()
    file(STRINGS "${VENG_SLANG_TAG_HEADER_DIR}/slang/slang-tag-version.h" versionLine
            REGEX "#define SLANG_VERSION_NUMERIC")
    string(REGEX MATCH "\"([^\"]*)\"" _ "${versionLine}")
    if (NOT CMAKE_MATCH_1 STREQUAL VENG_HOST_SHADER_SLANG_VERSION)
        message(WARNING "veng_add_host_shader_module: Slang is '${CMAKE_MATCH_1}', but host modules "
                "were validated against ${VENG_HOST_SHADER_SLANG_VERSION}; re-check the "
                "host-against-GPU agreement before relying on its bound")
    endif ()
endfunction()

function(veng_add_host_shader_module TARGET_NAME)
    cmake_parse_arguments(ARG "" "MODULE;NAME;SHADER_INCLUDE" "DEPENDS" ${ARGN})

    if (NOT ARG_MODULE)
        message(FATAL_ERROR "veng_add_host_shader_module(${TARGET_NAME}): MODULE is required")
    endif ()
    if (NOT ARG_NAME)
        message(FATAL_ERROR "veng_add_host_shader_module(${TARGET_NAME}): NAME is required")
    endif ()
    if (NOT ARG_NAME MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR
                "veng_add_host_shader_module(${TARGET_NAME}): NAME '${ARG_NAME}' is not a C++ identifier")
    endif ()
    _veng_check_host_shader_slang_version()

    cmake_path(ABSOLUTE_PATH ARG_MODULE
            BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} NORMALIZE
            OUTPUT_VARIABLE MODULE_ABS)
    set(SHADER_INCLUDE ${VENG_CORE_SHADER_DIR})
    if (ARG_SHADER_INCLUDE)
        cmake_path(ABSOLUTE_PATH ARG_SHADER_INCLUDE
                BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} NORMALIZE
                OUTPUT_VARIABLE SHADER_INCLUDE)
    endif ()

    set(OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}-host)
    set(OUT_SOURCE ${OUT_DIR}/${ARG_NAME}.cpp)
    set(OUT_HEADER ${OUT_DIR}/${ARG_NAME}.h)
    set(SHADER_INCLUDE_ARGS)
    if (SHADER_INCLUDE)
        set(SHADER_INCLUDE_ARGS --shader-include ${SHADER_INCLUDE})
    endif ()

    add_custom_command(
            OUTPUT ${OUT_SOURCE} ${OUT_HEADER}
            COMMAND $<TARGET_FILE:vengc> compile-host ${MODULE_ABS} --name ${ARG_NAME}
                    --out-dir ${OUT_DIR} ${SHADER_INCLUDE_ARGS} --depfile ${OUT_SOURCE}.d
            DEPENDS vengc ${MODULE_ABS} ${ARG_DEPENDS}
            DEPFILE ${OUT_SOURCE}.d
            COMMENT "Compiling host shader module ${ARG_NAME}")

    add_library(${TARGET_NAME} STATIC ${OUT_SOURCE} ${OUT_HEADER})
    target_include_directories(${TARGET_NAME} PUBLIC $<BUILD_INTERFACE:${OUT_DIR}>)
    target_link_libraries(${TARGET_NAME} PUBLIC glm::glm)
    set_target_properties(${TARGET_NAME} PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON
            POSITION_INDEPENDENT_CODE ON
            SKIP_LINTING ON
            DISABLE_PRECOMPILE_HEADERS ON)
    if (MSVC)
        target_compile_options(${TARGET_NAME} PRIVATE /WX- /fp:precise)
    else ()
        target_compile_options(${TARGET_NAME} PRIVATE -Wno-error -fno-fast-math -ffp-contract=off)
    endif ()
endfunction()
