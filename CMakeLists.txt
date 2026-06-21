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

# Always emit compile_commands.json so clang-tidy and editor tooling can find the
# per-TU flags, whether or not the in-build linter (below) is enabled.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/AssetPack.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Game.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Editor.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Docs.cmake)

option(VE_DEBUG "Enable veng debug features (validation layers, asserts)" OFF)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

find_package(Vulkan REQUIRED)

# glm (header-only). PUBLIC in veng's link interface (vector/matrix types in the
# public headers), so GLM_BUILD_INSTALL is forced ON: glm must export itself for
# install(EXPORT vengTargets) to resolve glm::glm and for find_package(veng) to
# be self-contained. GLM_BUILD_LIBRARY OFF keeps it header-only, as veng uses it.
set(GLM_BUILD_LIBRARY OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(
        glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG 1.0.1
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glm)

# GLFW. Linked PRIVATE (hidden behind the Native idiom), so it stays out of
# veng's interface; GLFW_INSTALL is ON only so find_dependency(glfw3) in the
# installed veng-config resolves, mirroring nfd.
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glfw)

# zlib (tinyexr's zip compression). Linked PRIVATE and never exported, so its
# install is skipped; the static target carries the include dirs tinyexr needs.
# find_package(ZLIB) synthesizes ZLIB::ZLIB; zlib's own CMake does not, so alias it.
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(
        zlib
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG v1.3.1
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(zlib)
add_library(ZLIB::ZLIB ALIAS zlibstatic)

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

# tinyexr (header used via engine/src/Vendor/TinyExr.cpp; its own targets are not built)
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
# veng library itself via engine/src/Vendor/ImGui.cpp (a source-include aggregation
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
# clang-tidy (opt-in)
# ---------------------------------------------------------------------------

# Set after the FetchContent deps above are configured and before veng's own
# subdirectories below, so CMAKE_CXX_CLANG_TIDY is inherited only by veng's
# targets — third-party sources built above are never tidied.
option(VENG_ENABLE_CLANG_TIDY "Run clang-tidy on veng's own sources during the build" OFF)

if (VENG_ENABLE_CLANG_TIDY)
    find_program(VENG_CLANG_TIDY NAMES clang-tidy)
    if (VENG_CLANG_TIDY)
        set(CMAKE_CXX_CLANG_TIDY "${VENG_CLANG_TIDY}")
        message(STATUS "clang-tidy enabled: ${VENG_CLANG_TIDY}")
    else ()
        message(WARNING "VENG_ENABLE_CLANG_TIDY is ON but clang-tidy was not found; not linting.")
    endif ()
endif ()

# ---------------------------------------------------------------------------
# Libraries
# ---------------------------------------------------------------------------

# assetpack first: engine links veng::assetpack, so the target must exist
# before engine/CMakeLists.txt references it.
add_subdirectory(assetpack)
add_subdirectory(engine)
add_subdirectory(cooker)

# The editor framework library is top-level-only (same gate as tests/examples):
# it serves the example editor and is not part of the installed library surface.
if (PROJECT_IS_TOP_LEVEL)
    add_subdirectory(editor)
endif ()

# ---------------------------------------------------------------------------
# Core pack — cook built-in vertex layouts and embed into libveng
# (only when vengc is available, i.e. veng is the top-level project)
# ---------------------------------------------------------------------------

# Cooked by the veng-free bootstrap cooker, NOT vengc: vengc links veng::veng
# (the module-reflection dependency inversion), so cooking veng's own embedded
# core pack with vengc would form a build cycle (veng -> core-pack cook -> vengc
# -> veng). veng_cook_bootstrap links only the cooker's veng-free core, so veng
# can depend on it.
if (TARGET veng_cook_bootstrap)
    set(VENG_CORE_PACK_JSON ${CMAKE_CURRENT_SOURCE_DIR}/engine/assets/core/core.vengpack.json)
    set(VENG_CORE_PACK_BIN  ${CMAKE_CURRENT_BINARY_DIR}/core.vengpack)
    set(VENG_CORE_PACK_CPP  ${CMAKE_CURRENT_BINARY_DIR}/generated/CoreLayoutPack.cpp)

    # veng_cook_bootstrap writes a depfile of every source the cook read (the
    # manifest, the vertex-layout JSONs, and each shader's .slang and its
    # includes via Slang's dependency list); DEPFILE drives the re-cook so the
    # sources need not be restated here.
    add_custom_command(
        OUTPUT ${VENG_CORE_PACK_BIN}
        COMMAND $<TARGET_FILE:veng_cook_bootstrap> cook ${VENG_CORE_PACK_JSON} -o ${VENG_CORE_PACK_BIN} --depfile ${VENG_CORE_PACK_BIN}.d
        DEPENDS veng_cook_bootstrap ${VENG_CORE_PACK_JSON}
        DEPFILE ${VENG_CORE_PACK_BIN}.d
        COMMENT "Cooking veng core pack")

    add_custom_command(
        OUTPUT ${VENG_CORE_PACK_CPP}
        COMMAND ${CMAKE_COMMAND}
                -DINPUT=${VENG_CORE_PACK_BIN}
                -DOUTPUT=${VENG_CORE_PACK_CPP}
                -DSYMBOL=g_CoreLayoutPack
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedBinary.cmake
        DEPENDS ${VENG_CORE_PACK_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedBinary.cmake
        COMMENT "Embedding veng core pack")

    # GENERATED so the build doesn't expect it up front; SKIP_LINTING so the
    # opt-in clang-tidy pass never walks the multi-megabyte embedded byte array.
    set_source_files_properties(${VENG_CORE_PACK_CPP} PROPERTIES
            GENERATED TRUE
            SKIP_LINTING TRUE)

    # Custom target so veng (in a subdirectory scope) can depend on the
    # generated file produced by a command in this (root) scope.
    add_custom_target(veng_core_pack_gen DEPENDS ${VENG_CORE_PACK_CPP})

    target_sources(veng PRIVATE ${VENG_CORE_PACK_CPP})
    add_dependencies(veng veng_core_pack_gen)
    target_compile_definitions(veng PRIVATE VENG_HAS_CORE_PACK=1)
endif ()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

option(VENG_BUILD_TESTS "Build the test suite" ${PROJECT_IS_TOP_LEVEL})

if (VENG_BUILD_TESTS)
    enable_testing()

    # doctest: single-header unit framework, test-only. Header-only INTERFACE
    # target doctest::doctest; its own tests/install are off (we only consume
    # the header + the CTest discovery script). Kept out of veng's exported
    # surface — never a dependency of the library itself.
    set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
    set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
            doctest
            GIT_REPOSITORY https://github.com/doctest/doctest.git
            GIT_TAG v2.4.12
            GIT_SHALLOW TRUE
            EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(doctest)
    include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)

    # ---- Unit band (pure-logic, type-mapping, barrier, assetpack — plans 02-04) --
    # Single executable hosting the framework-driven cases. main.cpp provides
    # DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN; each band adds its own *.cpp. Links
    # veng::veng (which itself links veng::assetpack PUBLIC) + doctest. NOTE:
    # -fno-exceptions is PRIVATE to veng/assetpack, so this target compiles
    # with exceptions enabled (doctest needs them).
    add_executable(veng_unit
            tests/unit/main.cpp
            tests/unit/sanity.cpp
            tests/unit/pure_logic.cpp
            tests/unit/task_system.cpp
            tests/unit/type_mapping.cpp
            tests/unit/barrier_decision.cpp
            tests/unit/transient_allocation.cpp
            tests/unit/asset_archive.cpp
            tests/unit/asset_manager.cpp
            tests/unit/primitives.cpp
            tests/unit/bounds.cpp
            tests/unit/visibility.cpp
            tests/unit/frustum.cpp
            tests/unit/bvh.cpp
            tests/unit/shadow_cascades.cpp
            tests/unit/punctual_shadows.cpp
            tests/unit/hi_z_history.cpp
            tests/unit/scene_ecs.cpp
            tests/unit/scene_queries.cpp
            tests/unit/scene_version.cpp
            tests/unit/hierarchy.cpp
            tests/unit/scene_broadphase.cpp
            tests/unit/reflection.cpp
            tests/unit/reflection_variant.cpp
            tests/unit/camera.cpp
            tests/unit/material_params.cpp
    )
    target_link_libraries(veng_unit PRIVATE veng::veng doctest::doctest)

    # Some unit cases reach into engine internals under src/ (the backend
    # TypeMapping header below); that path is on the include search list so they
    # compile. The public reflection serializer is on the include/ tree.
    target_include_directories(veng_unit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/engine/src)


    # The type-mapping (and barrier-decision, plan 04) cases include backend
    # headers (TypeMapping.h → Backend/Vulkan.h, which also pulls in GLFW + VMA),
    # so this target needs those *include dirs* — headers only, no run-time
    # driver dependency, so the pure-logic cases still run with no ICD. This is
    # allowed: include-hygiene guards *public* headers, not test use of backend
    # headers.
    target_link_libraries(veng_unit PRIVATE
            Vulkan::Vulkan
            glfw
            GPUOpen::VulkanMemoryAllocator
    )

    # Discover each TEST_CASE as its own ctest entry, labelled `unit` so
    # `ctest -L unit` is the fast, driver-free feedback loop.
    doctest_discover_tests(veng_unit PROPERTIES LABELS unit)

    # ---- Shared GPU-band support (consumed by the death + GPU bands) --------
    # Vulkan-driver probe so a GPU test can skip (not fail) where no ICD exists.
    # Links Vulkan PRIVATE; its public header pulls in no Vulkan headers, so
    # consumers include <support/GpuProbe.h> without the backend include graph.
    #
    # GpuContext.cpp (plan 06) adds the shared headless-Context RAII wrapper and
    # pixel-compare helper used by the one-exe GPU tests; it's written against
    # public veng APIs only, so this target now also links veng::veng.
    add_library(veng_test_support STATIC tests/support/GpuProbe.cpp tests/support/GpuContext.cpp)
    target_include_directories(veng_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(veng_test_support PUBLIC veng::veng)
    target_link_libraries(veng_test_support PRIVATE Vulkan::Vulkan)

    # ---- Death band (VE_ASSERT contract — plans 01 + 05) -------------------
    # VE_ASSERT aborts, which doctest can't trap in-process, so death cases run
    # as separate processes. The harness traps SIGABRT and converts the abort
    # into a clean exit once the assert message is on stderr; each case is a
    # separate add_test pinned to that message via PASS_REGULAR_EXPRESSION (see
    # death_main.cpp for why this, and not WILL_FAIL). GPU-coupled cases link the
    # driver probe and use SKIP_RETURN_CODE so they skip (not fail) with no ICD.
    add_executable(veng_death tests/death/death_main.cpp)
    target_link_libraries(veng_death PRIVATE veng::veng veng_test_support)
    # tovk_unmapped includes TypeMapping.h (→ Backend/Vulkan.h → GLFW + VMA);
    # headers only, like veng_unit.
    target_link_libraries(veng_death PRIVATE
            Vulkan::Vulkan
            glfw
            GPUOpen::VulkanMemoryAllocator
    )

    # Pure-logic death cases (no device; run with no ICD).
    add_test(NAME death.sentinel COMMAND veng_death sentinel)
    set_tests_properties(death.sentinel PROPERTIES LABELS death
            PASS_REGULAR_EXPRESSION "sentinel death case")

    add_test(NAME death.vertex_format_unknown COMMAND veng_death vertex_format_unknown)
    set_tests_properties(death.vertex_format_unknown PROPERTIES LABELS death
            PASS_REGULAR_EXPRESSION "Unknown vertex element Format")

    add_test(NAME death.tovk_unmapped COMMAND veng_death tovk_unmapped)
    set_tests_properties(death.tovk_unmapped PROPERTIES LABELS death
            PASS_REGULAR_EXPRESSION "ToVk\\(Format\\): unmapped format")

    add_test(NAME death.assert_message COMMAND veng_death assert_message)
    set_tests_properties(death.assert_message PROPERTIES LABELS death
            PASS_REGULAR_EXPRESSION "assert_message case fired")

    add_test(NAME death.scene_get_stale_entity COMMAND veng_death scene_get_stale_entity)
    set_tests_properties(death.scene_get_stale_entity PROPERTIES LABELS death
            PASS_REGULAR_EXPRESSION "dead or stale entity")

    add_test(NAME death.scene_get_missing_component COMMAND veng_death scene_get_missing_component)
    set_tests_properties(death.scene_get_missing_component PROPERTIES LABELS death
            PASS_REGULAR_EXPRESSION "lacks the component")

    add_test(NAME death.type_id_collision COMMAND veng_death type_id_collision)
    set_tests_properties(death.type_id_collision PROPERTIES LABELS death
            PASS_REGULAR_EXPRESSION "TypeId collision")

    add_test(NAME death.transform_parent_cycle COMMAND veng_death transform_parent_cycle)
    set_tests_properties(death.transform_parent_cycle PROPERTIES LABELS death
            PASS_REGULAR_EXPRESSION "parent is a descendant of child")

    add_test(NAME death.transform_parent_dead COMMAND veng_death transform_parent_dead)
    set_tests_properties(death.transform_parent_dead PROPERTIES LABELS death
            PASS_REGULAR_EXPRESSION "Hierarchy references a dead or stale entity")

    add_test(NAME death.spot_shadow_range_nonpositive COMMAND veng_death spot_shadow_range_nonpositive)
    set_tests_properties(death.spot_shadow_range_nonpositive PROPERTIES LABELS death
            PASS_REGULAR_EXPRESSION "ComputeSpotShadowView needs range > 0")

    add_test(NAME death.spot_shadow_cone_out_of_range COMMAND veng_death spot_shadow_cone_out_of_range)
    set_tests_properties(death.spot_shadow_cone_out_of_range PROPERTIES LABELS death
            PASS_REGULAR_EXPRESSION "ComputeSpotShadowView needs outerCone")

    add_test(NAME death.point_shadow_range_nonpositive COMMAND veng_death point_shadow_range_nonpositive)
    set_tests_properties(death.point_shadow_range_nonpositive PROPERTIES LABELS death
            PASS_REGULAR_EXPRESSION "ComputePointShadowView needs range > 0")

    # GPU-coupled death cases (need a headless Context; skip with no driver).
    add_test(NAME death.buffer_upload_overrun COMMAND veng_death buffer_upload_overrun)
    set_tests_properties(death.buffer_upload_overrun PROPERTIES LABELS "gpu;death"
            SKIP_RETURN_CODE 77 PASS_REGULAR_EXPRESSION "upload out of range")

    add_test(NAME death.index_u16_into_u32 COMMAND veng_death index_u16_into_u32)
    set_tests_properties(death.index_u16_into_u32 PROPERTIES LABELS "gpu;death"
            SKIP_RETURN_CODE 77 PASS_REGULAR_EXPRESSION "cannot upload u16 indices")

    add_test(NAME death.index_u32_into_u16 COMMAND veng_death index_u32_into_u16)
    set_tests_properties(death.index_u32_into_u16 PROPERTIES LABELS "gpu;death"
            SKIP_RETURN_CODE 77 PASS_REGULAR_EXPRESSION "cannot upload u32 indices")

    add_test(NAME death.descriptor_type_mismatch COMMAND veng_death descriptor_type_mismatch)
    set_tests_properties(death.descriptor_type_mismatch PROPERTIES LABELS "gpu;death"
            SKIP_RETURN_CODE 77 PASS_REGULAR_EXPRESSION "not a sampled or storage image")

    # Include-hygiene guard: compiles every public header while linking only the
    # PUBLIC deps of veng (glm, fmt, ImGui). Because Vulkan/GLFW/VMA/nfd link
    # PRIVATE, this fails to build if a public header leaks a backend header.
    # Also links veng_editor::veng_editor so the sweep covers the VengEditor/
    # public headers: it propagates their include dir and the editor's PUBLIC deps
    # (glm/fmt/ImGui via veng) while keeping imnodes and nlohmann PRIVATE, so a
    # leaked backend include in an editor public header fails this build too.
    add_executable(veng_include_hygiene tests/include_hygiene.cpp)
    target_link_libraries(veng_include_hygiene PRIVATE veng::veng veng_editor::veng_editor)

    add_test(NAME include_hygiene COMMAND veng_include_hygiene)

    # ---- Editor unit band (device-free) -------------------------------------
    # The NodeGraph topology core and the rest of libveng_editor's pure logic.
    # veng_unit links only veng::veng, so the editor symbols need their own
    # device-free target: it links veng_editor::veng_editor (which links veng::veng
    # PUBLIC) + doctest, with editor/src on the include path so cases can reach the
    # editor's src-private headers. Runs under the same no-ICD band as veng_unit.
    add_executable(veng_editor_unit
            tests/unit/main.cpp
            tests/editor/node_graph.cpp
            tests/editor/node_catalog_serialize.cpp
            tests/editor/material_catalog_compile.cpp
            tests/editor/field_widget.cpp
            tests/editor/asset_source_index.cpp
    )
    # The serialization cases drive the JSON object directly, so the test target
    # links nlohmann/json (PRIVATE in libveng_editor — never exposed in a header).
    target_link_libraries(veng_editor_unit PRIVATE veng_editor::veng_editor doctest::doctest
            nlohmann_json::nlohmann_json)
    target_compile_definitions(veng_editor_unit PRIVATE JSON_NOEXCEPTION)
    target_include_directories(veng_editor_unit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/editor/src)

    doctest_discover_tests(veng_editor_unit PROPERTIES LABELS unit)

    # ---- Module ABI + loader (plan 01, driver-free) -------------------------
    # A real game module and a wrong-version variant, each a SHARED library
    # linking veng::veng and exporting VengModuleRegister + VengModuleAbiVersion.
    # bad_version_module forces a mismatched ABI version so the loader rejects it.
    # loader_test loads both through the real platform loader by the path CMake
    # bakes in, proving the load -> version-check -> register round-trip.
    add_library(veng_test_module SHARED tests/module/test_module.cpp)
    target_link_libraries(veng_test_module PRIVATE veng::veng)

    add_library(veng_bad_version_module SHARED tests/module/bad_version_module.cpp)
    target_link_libraries(veng_bad_version_module PRIVATE veng::veng)
    target_compile_definitions(veng_bad_version_module PRIVATE VENG_MODULE_ABI_VERSION=999999u)

    add_executable(veng_loader_test tests/module/loader_test.cpp)
    target_link_libraries(veng_loader_test PRIVATE veng::veng)
    add_dependencies(veng_loader_test veng_test_module veng_bad_version_module)
    target_compile_definitions(veng_loader_test PRIVATE
            VENG_TEST_MODULE_PATH="$<TARGET_FILE:veng_test_module>"
            VENG_BAD_VERSION_MODULE_PATH="$<TARGET_FILE:veng_bad_version_module>"
    )

    add_test(NAME loader_test COMMAND veng_loader_test)
    set_tests_properties(loader_test PROPERTIES LABELS module)

    # Fuzzy image comparator for the golden smoke test (smoke_golden, registered
    # after the examples). Links no veng/Vulkan — just stb_image, which decodes
    # both the PPM capture and the PNG golden — so it runs with or without a GPU.
    add_executable(veng_golden_compare tests/golden_compare.cpp)
    target_include_directories(veng_golden_compare PRIVATE ${stb_SOURCE_DIR})

    # Headless smoke test: brings up a windowless context, renders off-screen and
    # verifies downloaded pixels. Skips (SKIP_RETURN_CODE 77) with no Vulkan ICD.
    add_executable(veng_headless_smoke tests/headless_smoke.cpp)
    target_link_libraries(veng_headless_smoke PRIVATE veng::veng veng_test_support)

    add_test(NAME headless_smoke COMMAND veng_headless_smoke)
    set_tests_properties(headless_smoke PROPERTIES LABELS gpu SKIP_RETURN_CODE 77)

    # Compute dispatch test (plan 05): headless graphics -> compute -> graphics
    # render graph proving cmd.Dispatch and the storage-image barrier chain.
    # Skips (SKIP_RETURN_CODE 77) with no Vulkan ICD.
    # Cooks the GPU tests' Slang shaders into a small .vengpack. The tests mount
    # it and LoadSync<ShaderAsset> their modules exactly as an app would — the
    # engine has no runtime Slang compiler, so cooking is the only path to a
    # shader module. None of these shaders declare vertex inputs, so the pack
    # needs no REFERENCE for a vertex-layout id.
    add_asset_pack(veng_test_shaders
            PACK   tests/shaders/test.vengpack.json
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/test_shaders.vengpack
            DEPENDS
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/invert.comp.slang
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/fullscreen.vert.slang
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/sample.frag.slang
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/bindless_sample.frag.slang
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/indirect_instance.vert.slang
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/indirect_instance.frag.slang
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/indirect_cull.comp.slang
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/invert.comp.shader.json
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/fullscreen.vert.shader.json
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/sample.frag.shader.json
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/bindless_sample.frag.shader.json
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/indirect_instance.vert.shader.json
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/indirect_instance.frag.shader.json
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/indirect_cull.comp.shader.json)

    add_executable(veng_compute_dispatch tests/compute_dispatch.cpp)
    target_link_libraries(veng_compute_dispatch PRIVATE veng::veng veng_test_support)
    add_dependencies(veng_compute_dispatch veng_test_shaders)
    target_compile_definitions(veng_compute_dispatch PRIVATE
            TEST_SHADER_PACK="${CMAKE_CURRENT_BINARY_DIR}/test_shaders.vengpack"
    )

    add_test(NAME compute_dispatch COMMAND veng_compute_dispatch)
    set_tests_properties(compute_dispatch PROPERTIES LABELS gpu SKIP_RETURN_CODE 77)

    # ---- In-process multi-case GPU integration suite (plan 05, area 5b) ----
    # A single doctest-based executable running many GPU cases, each standing
    # up and tearing down its own Context (GpuFixture, tests/gpu/fixture.h) for
    # real per-case isolation — something the old Context singleton made
    # impossible. Ports the focused GPU exercises from planset-3 plan 06
    # (buffer/typed-buffer roundtrip, image clear+format, descriptor write
    # paths) plus a new per-case isolation proof. Custom main (tests/gpu/main.cpp)
    # checks Test::HasVulkanDriver() first and returns 77 with no ICD; one ctest
    # entry, labelled `gpu`, same skip contract as the rest of the band.
    add_executable(veng_gpu
            tests/gpu/main.cpp
            tests/gpu/buffer_roundtrip.cpp
            tests/gpu/typed_buffer_roundtrip.cpp
            tests/gpu/image_clear_format.cpp
            tests/gpu/descriptor_write_paths.cpp
            tests/gpu/isolation.cpp
            tests/gpu/bindless_registry.cpp
            tests/gpu/ring_buffered_material.cpp
            tests/gpu/timeline_semaphore.cpp
            tests/gpu/transfer_retire.cpp
            tests/gpu/async_upload.cpp
            tests/gpu/async_mesh_build.cpp
            tests/gpu/transient_aliasing.cpp
            tests/gpu/indirect_draw.cpp
            tests/gpu/adopt.cpp
            tests/gpu/create_async.cpp
            tests/gpu/scene_renderer.cpp
            tests/gpu/hi_z_pyramid.cpp
            tests/gpu/occlusion_test.cpp
            tests/gpu/prefab_spawn.cpp
    )
    target_link_libraries(veng_gpu PRIVATE veng::veng veng_test_support doctest::doctest)
    # transfer_retire.cpp reaches through Native.h (raw vk::/VMA handles via the
    # public escape hatch) to drive RetireOnTransfer/ReleaseBuffer, so this target
    # needs the backend headers' *include dirs* (Vulkan + GLFW + VMA). Allowed:
    # include-hygiene guards public headers, not test use of the Native escape hatch.
    target_link_libraries(veng_gpu PRIVATE
            Vulkan::Vulkan
            glfw
            GPUOpen::VulkanMemoryAllocator
    )
    # bindless_registry.cpp and texture_loader.cpp draw with the test shaders
    # (fullscreen.vert + bindless_sample.frag), loaded by id from the cooked pack.
    add_dependencies(veng_gpu veng_test_shaders)
    target_compile_definitions(veng_gpu PRIVATE
            TEST_SHADER_PACK="${CMAKE_CURRENT_BINARY_DIR}/test_shaders.vengpack"
    )

    # texture_loader.cpp cooks a fixture pack in-process (plan 06), so it needs
    # the cooker library and the cooker fixtures' directory.
    if (VENG_BUILD_TOOLS)
        target_sources(veng_gpu PRIVATE
                tests/gpu/texture_loader.cpp
                tests/gpu/mesh_loader.cpp
                tests/gpu/shader_loader.cpp
                tests/gpu/material_loader.cpp
                tests/gpu/vertex_layout_loader.cpp
        )
        target_link_libraries(veng_gpu PRIVATE veng::cook)
        target_compile_definitions(veng_gpu PRIVATE
                GPU_COOKER_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/cooker/fixtures"
                # The deferred g-buffer oracle (scene_renderer.cpp) cooks this pack
                # in-process: a brick material on the core canonical layout, drawn
                # through the SceneRenderer to assert the albedo blit.
                GPU_GBUFFER_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/assets"
                # The PostProcess fullscreen-material proof (scene_renderer.cpp)
                # cooks an identity PostProcess material and drives it through a
                # PostProcessScenePass to assert it samples an upstream target and
                # writes it through unchanged.
                GPU_POSTPROCESS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/assets"
        )
    endif ()

    add_test(NAME veng_gpu COMMAND veng_gpu)
    set_tests_properties(veng_gpu PROPERTIES LABELS gpu SKIP_RETURN_CODE 77)

    # ---- MaterialPreview surface (editor src) ------------------------------
    # The one GPU target that links libveng_editor: it constructs a
    # VengEditor::MaterialPreview (which owns a SceneRenderer + a Ref<ImGuiTexture>),
    # so it needs a windowed Context and the ImGui Vulkan backend. It cooks the brick
    # g-buffer fixture in-process (veng::cook), so it is gated on VENG_BUILD_TOOLS and
    # the editor library. A second viewport-style SceneRenderer is recorded alongside
    # the preview, so the two-renderer cross-graph handoff runs under the validation
    # gate. Skips (SKIP_RETURN_CODE 77) with no Vulkan ICD.
    if (VENG_BUILD_TOOLS AND TARGET veng_editor)
        add_executable(veng_material_preview tests/material_preview.cpp)
        target_link_libraries(veng_material_preview PRIVATE
                veng::veng veng_editor veng::cook veng_test_support)
        # The test includes the editor's src-private MaterialPreview.h.
        target_include_directories(veng_material_preview PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/editor/src)
        target_compile_definitions(veng_material_preview PRIVATE
                GPU_GBUFFER_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/assets")

        add_test(NAME veng_material_preview COMMAND veng_material_preview)
        set_tests_properties(veng_material_preview PROPERTIES LABELS gpu SKIP_RETURN_CODE 77)
    endif ()
endif ()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------

option(VENG_BUILD_EXAMPLES "Build the sample applications" ${PROJECT_IS_TOP_LEVEL})

if (VENG_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif ()

# Golden smoke test: render the hello-triangle scene headless and compare it
# against the checked-in golden. Runs through the launcher (which dlopens
# libhello_triangle), the real shipping path. Needs the launcher and the test
# comparator, so it's registered here, after the examples subdirectory defines
# hello_triangle-launcher. Labelled `gpu`; the script skips cleanly with no
# Vulkan ICD.
# ---- Cooker band (`-L cooker`) ------------------------------------------------
# The cooker suite is its own executable + label, separate from `veng_unit`'s
# `unit` band: it links veng::cook (which links veng::veng for module reflection)
# and the module-reflection test loads the real libhello_triangle, so it is
# registered after the examples subdirectory defines that module. Like veng_unit
# it compiles with exceptions (doctest needs them); -fno-exceptions is PRIVATE to
# veng/cook.
if (VENG_BUILD_TESTS AND VENG_BUILD_TOOLS AND TARGET hello_triangle)
    add_executable(veng_cooker
            tests/unit/main.cpp
            tests/cooker/cook_roundtrip.cpp
            tests/cooker/texture_cook.cpp
            tests/cooker/mesh_cook.cpp
            tests/cooker/shader_cook.cpp
            tests/cooker/material_cook.cpp
            tests/cooker/vertex_layout_cook.cpp
            tests/cooker/verify.cpp
            tests/cooker/module_reflect.cpp
            tests/cooker/prefab_cook.cpp
            tests/cooker/cook_source.cpp
            tests/cooker/material_compile_cook.cpp
    )
    # The cook-through test calls CompileMaterialGraph (editor) and cooks its
    # emitted .vmat through the real importer, so the suite links libveng_editor
    # in addition to libveng_cook (no cycle: the cooker is never linked by the
    # editor framework). editor/src is on the include path for the Layer-3 headers.
    target_link_libraries(veng_cooker PRIVATE veng::cook veng_editor::veng_editor doctest::doctest)
    target_include_directories(veng_cooker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/editor/src)
    add_dependencies(veng_cooker hello_triangle veng_bad_version_module)
    target_compile_definitions(veng_cooker PRIVATE
            VENG_COOKER_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/cooker/fixtures"
            VENG_HELLO_TRIANGLE_MODULE_PATH="$<TARGET_FILE:hello_triangle>"
            VENG_BAD_VERSION_MODULE_PATH="$<TARGET_FILE:veng_bad_version_module>"
            VENG_HT_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/hello-triangle/assets"
            VENG_CORE_PACK_JSON="${CMAKE_CURRENT_SOURCE_DIR}/engine/assets/core/core.vengpack.json"
    )

    doctest_discover_tests(veng_cooker PROPERTIES LABELS cooker)
endif ()

if (VENG_BUILD_TESTS AND TARGET hello_triangle-launcher AND TARGET veng_golden_compare)
    add_test(NAME smoke_golden
        COMMAND ${CMAKE_COMMAND}
            -D VENG_SMOKE_BIN=$<TARGET_FILE:hello_triangle-launcher>
            -D VENG_COMPARE_BIN=$<TARGET_FILE:veng_golden_compare>
            -D VENG_PROBE_BIN=$<TARGET_FILE:veng_headless_smoke>
            -D VENG_GOLDEN=${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/hello_triangle_scene.png
            -D VENG_TMP=${CMAKE_CURRENT_BINARY_DIR}/smoke_golden_capture.ppm
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/SmokeGolden.cmake
    )
    set_tests_properties(smoke_golden PROPERTIES LABELS gpu)

    # Automated launcher smoke: exercises the real dlopen -> VengModuleRegister ->
    # registry -> Run() binary path end-to-end. Runs the launcher with HT_SMOKE
    # pointed at a temp PPM (so it renders headless and exits) and asserts exit 0.
    # SKIP_RETURN_CODE 77 matches the rest of the gpu band. The only test covering
    # the migration's headline shipping path.
    add_test(NAME hello_triangle_launcher_smoke
        COMMAND $<TARGET_FILE:hello_triangle-launcher>)
    set_tests_properties(hello_triangle_launcher_smoke PROPERTIES
        LABELS gpu SKIP_RETURN_CODE 77
        ENVIRONMENT "HT_SMOKE=${CMAKE_CURRENT_BINARY_DIR}/hello_triangle_launcher_smoke.ppm")

    # ---- Local validation-error gate ----------------------------------------
    # ctest -L validation: runs the gpu-labelled binaries that emit Vulkan
    # validation messages under VE_DEBUG and fails if a new [ERROR] Vulkan
    # validation line appears (see cmake/ValidationGate.cmake for the allowlist
    # of documented gaps). Includes the launcher path so the real shipping shape
    # runs under the gate too. Registered after the examples subdirectory so the
    # launcher target exists. Under the default (non-VE_DEBUG) build no validation
    # layers are enabled, so this is trivially green.
    # The MaterialPreview gate binary exists only under VENG_BUILD_TOOLS + editor;
    # pass its path through only when the target was built so the gate runs the
    # two-renderer handoff there too.
    set(VENG_GATE_MATERIAL_PREVIEW_ARG)
    if (TARGET veng_material_preview)
        set(VENG_GATE_MATERIAL_PREVIEW_ARG
            -D VENG_GATE_BIN_MATERIAL_PREVIEW=$<TARGET_FILE:veng_material_preview>)
    endif ()

    add_test(NAME validation_gate
        COMMAND ${CMAKE_COMMAND}
            -D VENG_GATE_BIN_HEADLESS_SMOKE=$<TARGET_FILE:veng_headless_smoke>
            -D VENG_GATE_BIN_COMPUTE_DISPATCH=$<TARGET_FILE:veng_compute_dispatch>
            -D VENG_GATE_BIN_GPU=$<TARGET_FILE:veng_gpu>
            -D VENG_GATE_BIN_LAUNCHER=$<TARGET_FILE:hello_triangle-launcher>
            ${VENG_GATE_MATERIAL_PREVIEW_ARG}
            -D VENG_GATE_LAUNCHER_CAPTURE=${CMAKE_CURRENT_BINARY_DIR}/validation_gate_launcher.ppm
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ValidationGate.cmake
    )
    set_tests_properties(validation_gate PROPERTIES LABELS validation)
endif ()

# ---------------------------------------------------------------------------
# Documentation
# ---------------------------------------------------------------------------

option(VENG_BUILD_DOCS "Add the Doxygen `docs` target" ${PROJECT_IS_TOP_LEVEL})

if (VENG_BUILD_DOCS)
    veng_add_docs()
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

install(DIRECTORY engine/include/Veng DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

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
