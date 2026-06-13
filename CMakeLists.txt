cmake_minimum_required(VERSION 4.1)
project(veng VERSION 0.1.0 LANGUAGES CXX)

# Note: not REQUIRED — AppleClang doesn't advertise the C++26 dialect flag,
# so the standard decays to the newest the compiler supports.
set(CMAKE_CXX_STANDARD 26)

# ImGui (and the rest of the public API) is exported from the shared library;
# consumers resolve ImGui:: symbols against libveng rather than compiling
# their own copy, so symbols must stay visible.
set(CMAKE_CXX_VISIBILITY_PRESET default)
set(CMAKE_VISIBILITY_INLINES_HIDDEN OFF)

include(FetchContent)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Shaders.cmake)

option(VE_DEBUG "Enable veng debug features (validation layers, asserts)" OFF)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

find_package(Vulkan REQUIRED)
find_package(glfw3 3.3 REQUIRED)
find_package(glm REQUIRED)
find_package(ZLIB REQUIRED) # tinyexr's zip compression

# fmtlib/fmt
# FMT_INSTALL/VMA_ENABLE_INSTALL/NFD_INSTALL: these deps appear in veng's
# exported link interface, so they must land in export sets of their own and
# install alongside veng for find_package(veng) to be self-contained.
set(FMT_INSTALL ON CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "Disable fmt tests" FORCE)
set(FMT_DOC OFF CACHE BOOL "Disable fmt docs" FORCE)
set(FMT_HEADER_ONLY OFF CACHE BOOL "Enable header-only mode" FORCE)
set(FMT_USE_LOCALE ON CACHE BOOL "Enable locale support" FORCE)
FetchContent_Declare(
        fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG 12.1.0
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(fmt)

# VulkanMemoryAllocator
set(VMA_ENABLE_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(
        VulkanMemoryAllocator
        GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
        GIT_TAG v3.4.0
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(VulkanMemoryAllocator)

# nativefiledialog-extended (provides the `nfd` target)
set(NFD_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(
        nativefiledialog_extended
        GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
        GIT_TAG v1.3.0
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nativefiledialog_extended)

# tinyexr (header used via src/Vendor/TinyExr.cpp; its own targets are not built)
FetchContent_Declare(
        tinyexr
        GIT_REPOSITORY https://github.com/syoyo/tinyexr.git
        GIT_TAG v1.0.12
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(tinyexr)

# stb (header-only); no release tags, pinned to a specific commit
FetchContent_Declare(
        stb
        GIT_REPOSITORY https://github.com/nothings/stb.git
        GIT_TAG 31c1ad37456438565541f4919958214b6e762fb4
)
FetchContent_MakeAvailable(stb)

# ImGui + imnodes. ImGui/imnodes sources are compiled exactly once, into the
# veng library itself via src/Vendor/ImGui.cpp (a source-include aggregation
# TU). imnodes' CMake only needs an imgui target for include propagation, so
# feed it a header-only INTERFACE target instead of compiling imgui twice.
FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.92.4-docking
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui_headers INTERFACE)
target_include_directories(imgui_headers INTERFACE ${imgui_SOURCE_DIR})
set(IMNODES_IMGUI_TARGET_NAME imgui_headers)

# Latest tag (v0.5, 2022) is far behind master (38 commits, as of 2026-06);
# pinned to a master commit instead.
FetchContent_Declare(
        imnodes
        GIT_REPOSITORY https://github.com/Nelarius/imnodes.git
        GIT_TAG eb36902c892548ef94f88f51ad7e7c9c7058a71c
        EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(imnodes)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

add_library(veng SHARED
        src/Application.cpp
        src/Assert.cpp
        src/Log.cpp
        src/Time.cpp
        src/Window.cpp
        src/ImGui/ImGuiLayer.cpp
        src/ImGui/ImGuiTexture.cpp
        src/Renderer/RenderGraph.cpp
        src/Renderer/Backend/Barrier.cpp
        src/Renderer/Backend/Buffer.cpp
        src/Renderer/Backend/CommandBuffer.cpp
        src/Renderer/Backend/CommandPool.cpp
        src/Renderer/Backend/ComputePipeline.cpp
        src/Renderer/Backend/Context.cpp
        src/Renderer/Backend/DebugMarkers.cpp
        src/Renderer/Backend/DescriptorPool.cpp
        src/Renderer/Backend/DescriptorSet.cpp
        src/Renderer/Backend/DescriptorSetLayout.cpp
        src/Renderer/Backend/Fence.cpp
        src/Renderer/Backend/GraphicsPipeline.cpp
        src/Renderer/Backend/Image.cpp
        src/Renderer/Backend/ImageView.cpp
        src/Renderer/Backend/PipelineLayout.cpp
        src/Renderer/Backend/Sampler.cpp
        src/Renderer/Backend/Semaphore.cpp
        src/Renderer/Backend/Shader.cpp
        src/Renderer/Backend/SwapChain.cpp
        src/Renderer/Backend/SynchronizationFrame.cpp
        src/Renderer/Backend/Utils.cpp
        src/Renderer/Backend/VertexBufferLayout.cpp
        src/Vendor/ImGui.cpp
        src/Vendor/StbImage.cpp
        src/Vendor/TinyExr.cpp
)
add_library(veng::veng ALIAS veng)

target_include_directories(veng PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# ImGui/imnodes headers are part of veng's public API surface
# (Veng/Vendor/ImGui.h includes <imgui.h>, <imnodes.h>, ...).
target_include_directories(veng PUBLIC
        $<BUILD_INTERFACE:${imgui_SOURCE_DIR}>
        $<BUILD_INTERFACE:${imnodes_SOURCE_DIR}>
)

# tinyexr/stb headers are implementation details of the vendor TUs.
target_include_directories(veng PRIVATE
        ${tinyexr_SOURCE_DIR}
        ${stb_SOURCE_DIR}
)

target_compile_definitions(veng PRIVATE VE_BUILD_SHARED)

# veng raises no exceptions (see Veng/Assert.h): asserts are fatal, recoverable
# operations return Veng::Result<T>. Building -fno-exceptions is the enforcement
# mechanism — any stray `throw` becomes a compile error. tinyexr is the one
# vendor TU that uses exceptions by default, so disable them for it too.
target_compile_options(veng PRIVATE -fno-exceptions)
set_source_files_properties(src/Vendor/TinyExr.cpp PROPERTIES
        COMPILE_DEFINITIONS TINYEXR_USE_EXCEPTIONS=0)

if (VE_DEBUG)
    target_compile_definitions(veng PUBLIC VE_ENABLE_VALIDATION_LAYERS=1 VE_DEBUG=1)
endif ()

# glm and fmt appear in veng's public headers (engine vector/matrix types and
# string formatting), so they stay PUBLIC. Vulkan, GLFW, VMA and nfd are hidden
# behind the Native idiom (see Veng/Renderer/Native.h) — no public header pulls
# them in, so they link PRIVATE and stay out of the consumer include graph.
target_link_libraries(veng PUBLIC
        glm::glm
        fmt::fmt
)

target_link_libraries(veng PRIVATE
        Vulkan::Vulkan
        glfw
        GPUOpen::VulkanMemoryAllocator
        nfd
        ZLIB::ZLIB
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

option(VENG_BUILD_TESTS "Build the test suite" ${PROJECT_IS_TOP_LEVEL})

if (VENG_BUILD_TESTS)
    enable_testing()

    # Include-hygiene guard: compiles every public header while linking only the
    # PUBLIC deps of veng (glm, fmt, ImGui). Because Vulkan/GLFW/VMA/nfd link
    # PRIVATE, this fails to build if a public header leaks a backend header.
    add_executable(veng_include_hygiene tests/include_hygiene.cpp)
    target_link_libraries(veng_include_hygiene PRIVATE veng::veng)

    add_test(NAME include_hygiene COMMAND veng_include_hygiene)

    # Headless smoke test: brings up a windowless context, renders off-screen and
    # verifies downloaded pixels. Requires a Vulkan ICD at run time.
    add_executable(veng_headless_smoke tests/headless_smoke.cpp)
    target_link_libraries(veng_headless_smoke PRIVATE veng::veng)

    add_test(NAME headless_smoke COMMAND veng_headless_smoke)

    # Compute dispatch test (plan 05): headless graphics -> compute -> graphics
    # render graph proving cmd.Dispatch and the storage-image barrier chain.
    # Requires a Vulkan ICD at run time.
    add_executable(veng_compute_dispatch tests/compute_dispatch.cpp)
    target_link_libraries(veng_compute_dispatch PRIVATE veng::veng)
    target_compile_definitions(veng_compute_dispatch PRIVATE
            CD_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders"
    )

    add_shaders(veng_compute_dispatch_shaders
            tests/shaders/invert.comp
            tests/shaders/fullscreen.vert
            tests/shaders/sample.frag
    )
    add_dependencies(veng_compute_dispatch veng_compute_dispatch_shaders)

    add_test(NAME compute_dispatch COMMAND veng_compute_dispatch)
endif ()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------

option(VENG_BUILD_EXAMPLES "Build the sample applications" ${PROJECT_IS_TOP_LEVEL})

if (VENG_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif ()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------

install(TARGETS veng
        EXPORT vengTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/Veng DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Third-party headers reachable from veng's public headers, so that an
# installed veng is self-contained for find_package consumers.
install(FILES
        ${imgui_SOURCE_DIR}/imgui.h
        ${imgui_SOURCE_DIR}/imgui_internal.h
        ${imgui_SOURCE_DIR}/imconfig.h
        ${imgui_SOURCE_DIR}/imstb_textedit.h
        ${imgui_SOURCE_DIR}/imstb_rectpack.h
        ${imgui_SOURCE_DIR}/imstb_truetype.h
        ${imnodes_SOURCE_DIR}/imnodes.h
        ${imnodes_SOURCE_DIR}/imnodes_internal.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(FILES
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.h
        ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/backends
)

install(FILES cmake/Shaders.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/veng)

install(EXPORT vengTargets
        FILE veng-targets.cmake
        NAMESPACE veng::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/veng
)

configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/veng-config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/veng-config.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/veng
)

write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/veng-config-version.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
)

install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/veng-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/veng-config-version.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/veng
)
