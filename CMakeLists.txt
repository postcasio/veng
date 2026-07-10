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

# Route compilation through ccache when present, so unchanged TUs are served from
# cache across rebuilds, clean builds, and branch switches. The #embed / bin2cpp
# machinery below is written to keep ccache's view of generated sources correct.
find_program(CCACHE_PROGRAM ccache)
if (CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif ()

include(FetchContent)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# The engine core-data paths, resolved to source-tree locations for an in-tree build.
# The uppercase VENG_CORE_* names are the engine-internal scaffolding the helper bodies
# and the top-level build read; the generated veng-config sets these to installed paths
# downstream. The lowercase veng_CORE_* aliases are the consumer-facing find_package
# result API a downstream veng_add_project(... REFERENCE ${veng_CORE_PACK_JSON}) reads —
# matching the veng_FOUND / veng_VERSION casing find_package(veng) generates. In-tree the
# two audiences resolve to the same source-tree paths.
#
# VENG_CORE_SHADER_DIR is threaded to every cook as `--shader-include` so a consumer (or
# generated) `.slang` resolves `#include "Veng/surface.slang"` against the engine header;
# veng_add_asset_pack / veng_add_project pass it, and the test cooks pass it too. The core
# pack itself cooks without it — its shaders reach the header through their own dir.
# VENG_CORE_PACK_JSON is the core pack's source manifest a downstream cook `--reference`s
# to resolve core-pack ids (the standard vertex shaders).
set(VENG_CORE_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/engine/assets/core/shaders"
        CACHE PATH "Engine core shader directory added to every cook's Slang search path")
set(VENG_CORE_PACK_JSON "${CMAKE_CURRENT_SOURCE_DIR}/engine/assets/core/core.vengpack.json"
        CACHE FILEPATH "Engine core pack source manifest referenced by a downstream cook")
set(veng_CORE_SHADER_DIR "${VENG_CORE_SHADER_DIR}")
set(veng_CORE_PACK_JSON "${VENG_CORE_PACK_JSON}")

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/AssetPack.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildConfig.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Project.cmake)
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

# zstd — archive-blob compression. Linked PUBLIC by assetpack: the runtime
# inflates a compressed blob on resolve, so the codec reaches libveng; the cooker
# gets the compressor transitively. zstd is a codec, not a source importer, so
# assetpack stays importer-free. Built static, install ON so libzstd_static joins
# veng's export set (assetpack exposes zstd in its PUBLIC interface). The repo's
# CMake lives under build/cmake.
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
        zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG v1.5.6
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR build/cmake
)
FetchContent_MakeAvailable(zstd)

# ImGui + imnodes. ImGui/imnodes sources are compiled exactly once, into the
# veng library itself via engine/src/Vendor/ImGui.cpp (a source-include aggregation
# TU). imnodes' CMake only needs an imgui target for include propagation, so
# feed it a header-only INTERFACE target instead of compiling imgui twice.
FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.92.8-docking
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

# nlohmann/json (header-only). PUBLIC in veng's link interface (the reflection JSON
# walker names json types), so JSON_Install is forced ON: nlohmann must export itself
# for install(EXPORT vengTargets) to resolve nlohmann_json::nlohmann_json and for
# find_package(veng) to be self-contained, matching glm/fmt/VMA/nfd. Declared and made
# available here — before add_subdirectory(assetpack)/add_subdirectory(engine) below —
# because engine/CMakeLists.txt links nlohmann_json::nlohmann_json PUBLIC on veng; the
# target must already exist when it does, or configure fails with an unknown-target error.
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install ON CACHE BOOL "" FORCE)
FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# Yoga — the flexbox layout solver behind Veng::Gui. Linked PRIVATE into libveng:
# its <yoga/*.h> headers are confined to engine/src/Gui/, so no public header names
# a Yoga type and the dependency stays out of veng's exported link interface (no
# install/export forcing, unlike the PUBLIC deps above). EXCLUDE_FROM_ALL builds
# only the `yogacore` target veng links, never Yoga's own tests/benchmarks, and
# keeps its CMake out of the default build so its warnings never trip veng's -Werror.
FetchContent_Declare(
        yoga
        GIT_REPOSITORY https://github.com/facebook/yoga.git
        GIT_TAG v3.2.1
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(yoga)

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
# Source encoding
# ---------------------------------------------------------------------------

# veng sources are BOM-less UTF-8. MSVC decodes narrow source files by the host
# codepage unless told otherwise, mangling the non-ASCII characters comments and
# test-name literals carry; /utf-8 pins both the source and execution charsets.
# Added at the same boundary as the warnings below so only veng's own targets
# inherit it.
if (MSVC)
    add_compile_options(/utf-8)
endif ()

# ---------------------------------------------------------------------------
# Warnings-as-errors under the debug build
# ---------------------------------------------------------------------------

# The debug build (VE_DEBUG) promotes compiler warnings to errors. Added here —
# after the FetchContent deps above and before veng's own subdirectories below —
# so it is inherited only by veng's targets; third-party sources built above are
# never affected (the same boundary the clang-tidy wiring relies on). The default
# (non-VE_DEBUG) build still merely warns.
if (VE_DEBUG)
    if (MSVC)
        add_compile_options(/WX)
    else ()
        add_compile_options(-Werror)
    endif ()
endif ()

# ---------------------------------------------------------------------------
# Libraries
# ---------------------------------------------------------------------------

# assetpack first: engine links veng::assetpack, so the target must exist
# before engine/CMakeLists.txt references it.
add_subdirectory(assetpack)
add_subdirectory(engine)

# The shared node-graph + material-codegen library is consumed by both the cooker
# (graph-sourced fragment cook) and the editor (graph UI + preview). veng is the tools:
# a from-source build always builds the cooker + graph, so this builds unconditionally
# (a library-only consumer uses find_package(veng) and gets the prebuilt imported tools).
add_subdirectory(graph)

# The optional MCP server library — an engine-tier lib a consumer opts into to expose
# its live systems to AI agents. Not linked by libveng; built unconditionally from
# source (like the cooker). It links veng::veng, so it comes after engine.
add_subdirectory(mcp)

add_subdirectory(cooker)

# The editor is the heavy GUI authoring surface above the cooker. The cooker + graph
# always build from source (veng is the tools); this option decides only whether the
# editor builds and installs, so a packaging superbuild produces a complete authoring
# SDK regardless of how the build was invoked. Defaults to a top-level build (the
# examples' editor), and is forced ON when the SDK is being installed.
option(VENG_INSTALL_SDK "Build and install the editor as part of the veng SDK" ${PROJECT_IS_TOP_LEVEL})

if (VENG_INSTALL_SDK OR PROJECT_IS_TOP_LEVEL)
    add_subdirectory(editor)
endif ()

# ---------------------------------------------------------------------------
# Binary embedding — #embed when the compiler supports it, bin2cpp otherwise
# ---------------------------------------------------------------------------

include(CheckCXXSourceCompiles)

# C++26 #embed splices a binary into a translation unit at compile time with no
# generated byte array. The probe below detects compiler support, but VENG_USE_EMBED
# gates whether veng actually takes that path; it defaults OFF so every embed runs
# through the bin2cpp host tool, whose generated byte array ccache tracks as a normal
# source dependency.
option(VENG_USE_EMBED "Use C++26 #embed for binary embedding when the compiler supports it" OFF)

if (VENG_USE_EMBED)
    # Probe for #embed; compilers without it (notably MSVC) take the bin2cpp path.
    # -Wno-c23-extensions keeps the probe green on clang, which accepts #embed but
    # flags it as a pre-C23 extension in C++ mode.
    set(CMAKE_REQUIRED_FLAGS "-Wno-c23-extensions")
    check_cxx_source_compiles("
static const unsigned char d[] = {
#embed __FILE__
};
int main() { return sizeof(d) == 0; }
" VENG_HAVE_EMBED)
    unset(CMAKE_REQUIRED_FLAGS)
else ()
    set(VENG_HAVE_EMBED OFF)
endif ()

# bin2cpp — host tool, built only when #embed is unavailable. Run during the
# build to generate the embedded core pack and default font as C++ byte arrays.
# A standalone executable with no veng/third-party deps; CXX_CLANG_TIDY is
# cleared because it is build infrastructure, not engine code.
if (NOT VENG_HAVE_EMBED)
    add_executable(bin2cpp ${CMAKE_CURRENT_SOURCE_DIR}/cmake/bin2cpp.cpp)
    set_target_properties(bin2cpp PROPERTIES CXX_CLANG_TIDY "")
endif ()

# veng_embed_binary(<input> <symbol>): compile a translation unit into veng that
# exposes Veng::<symbol>[] (unsigned char) and Veng::<symbol>Size (unsigned long)
# holding the bytes of <input>. Uses #embed when available, else bin2cpp. veng
# compiles in a subdirectory scope, so the per-asset target carries the ordering
# dependency on <input> (a custom-command output in this root scope) across.
function(veng_embed_binary input symbol)
    set(gen "${CMAKE_CURRENT_BINARY_DIR}/generated/embed_${symbol}.cpp")
    if (VENG_HAVE_EMBED)
        # Regenerate the shim at build time (cmake -P) whenever <input> changes, so
        # it carries a current SHA-256 of the embedded bytes. #embed splices those
        # bytes only at compile time and ccache does not track an #embed-ed file as
        # a dependency, so a changed pack is otherwise a false cache hit on the
        # unchanging shim source; the baked hash makes it a real cache miss. See
        # cmake/EmbedShim.cmake.
        add_custom_command(
                OUTPUT "${gen}"
                COMMAND ${CMAKE_COMMAND}
                        -DVENG_EMBED_INPUT=${input}
                        -DVENG_EMBED_SYMBOL=${symbol}
                        -DVENG_EMBED_OUTPUT=${gen}
                        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShim.cmake
                DEPENDS "${input}" ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShim.cmake
                COMMENT "Generating #embed shim for ${symbol}")
        # TARGET_DIRECTORY scopes these to where veng compiles the file (a
        # subdirectory), not this root scope. OBJECT_DEPENDS forces a recompile
        # when the embedded bytes change; the preprocessor splices the path
        # resolved at configure time. clang flags #embed as a pre-C23 extension.
        set_source_files_properties("${gen}" TARGET_DIRECTORY veng PROPERTIES
                GENERATED TRUE
                SKIP_LINTING TRUE
                OBJECT_DEPENDS "${input}"
                COMPILE_OPTIONS "-Wno-c23-extensions")
    else ()
        add_custom_command(
                OUTPUT "${gen}"
                COMMAND bin2cpp "${input}" "${gen}" "${symbol}"
                DEPENDS "${input}" bin2cpp
                COMMENT "Embedding ${symbol}")
        # GENERATED so the build doesn't expect it up front; SKIP_LINTING so the
        # opt-in clang-tidy pass never walks the multi-megabyte byte array.
        # TARGET_DIRECTORY scopes the properties to where veng compiles the file.
        set_source_files_properties("${gen}" TARGET_DIRECTORY veng PROPERTIES
                GENERATED TRUE
                SKIP_LINTING TRUE)
    endif ()

    target_sources(veng PRIVATE "${gen}")
    add_custom_target(veng_embed_${symbol} DEPENDS "${input}" "${gen}")
    add_dependencies(veng veng_embed_${symbol})
endfunction()

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
    set(VENG_CORE_PACK_BIN ${CMAKE_CURRENT_BINARY_DIR}/core.vengpack)

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

    veng_embed_binary(${VENG_CORE_PACK_BIN} g_CoreLayoutPack)
    target_compile_definitions(veng PRIVATE VENG_HAS_CORE_PACK=1)
endif ()

# ---------------------------------------------------------------------------
# Default UI font — embed Roboto into libveng as the engine default
# ---------------------------------------------------------------------------

# A plain file embed (no cook), so it has no dependency on the bootstrap cooker
# and is always available. ImGuiLayer loads it from memory as the default UI font
# when the consumer supplies no explicit font path.
set(VENG_DEFAULT_FONT_TTF ${CMAKE_CURRENT_SOURCE_DIR}/engine/fonts/roboto/Roboto.ttf)

veng_embed_binary(${VENG_DEFAULT_FONT_TTF} g_DefaultFont)
target_compile_definitions(veng PRIVATE VENG_HAS_DEFAULT_FONT=1)

# Icon font — embed FontAwesome Solid into libveng, merged onto the default UI
# font so the FontAwesome7.h ICON_FA_* glyphs are always available with no
# consumer-supplied font path.
set(VENG_ICON_FONT_OTF "${CMAKE_CURRENT_SOURCE_DIR}/engine/fonts/fontawesome-free-7.3.0/otfs/Font Awesome 7 Free-Solid-900.otf")

veng_embed_binary(${VENG_ICON_FONT_OTF} g_IconFont)
target_compile_definitions(veng PRIVATE VENG_HAS_ICON_FONT=1)

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
    add_executable(veng_test_unit
            tests/unit/main.cpp
            tests/unit/sanity.cpp
            tests/unit/animation.cpp
            tests/unit/pure_logic.cpp
            tests/unit/task_system.cpp
            tests/unit/type_mapping.cpp
            tests/unit/format_info.cpp
            tests/unit/barrier_decision.cpp
            tests/unit/render_graph_schedule.cpp
            tests/unit/input_events.cpp
            tests/unit/input_router.cpp
            tests/unit/action_resolve.cpp
            tests/unit/gamepad_adapter.cpp
            tests/unit/seat_routing.cpp
            tests/unit/seat_routing_integration.cpp
            tests/unit/seat_focus.cpp
            tests/unit/light_packing.cpp
            tests/unit/shader_interface.cpp
            tests/unit/transient_allocation.cpp
            tests/unit/asset_archive.cpp
            tests/unit/hex_id.cpp
            tests/unit/cooked_project.cpp
            tests/unit/launch_arguments.cpp
            tests/unit/asset_manager.cpp
            tests/unit/primitives.cpp
            tests/unit/bounds.cpp
            tests/unit/visibility.cpp
            tests/unit/frustum.cpp
            tests/unit/spherical_harmonics.cpp
            tests/unit/ibl_cube_sh.cpp
            tests/unit/bvh.cpp
            tests/unit/math_random.cpp
            tests/unit/math_ease.cpp
            tests/unit/platform_userpaths.cpp
            tests/unit/shadow_cascades.cpp
            tests/unit/punctual_shadows.cpp
            tests/unit/hi_z_history.cpp
            tests/unit/taa_jitter.cpp
            tests/unit/atmosphere.cpp
            tests/unit/sun_position.cpp
            tests/unit/scene_ecs.cpp
            tests/unit/scene_queries.cpp
            tests/unit/scene_version.cpp
            tests/unit/scene_clone.cpp
            tests/unit/scene_simulation.cpp
            tests/unit/hierarchy.cpp
            tests/unit/scene_broadphase.cpp
            tests/unit/net_replication.cpp
            tests/unit/net_state_flow.cpp
            tests/unit/net_input_flow.cpp
            tests/unit/net_join_flow.cpp
            tests/unit/reflection.cpp
            tests/unit/reflection_variant.cpp
            tests/unit/field_display.cpp
            tests/unit/field_widget_dispatch.cpp
            tests/unit/field_gate.cpp
            tests/unit/enum_reflection.cpp
            tests/unit/json_serialize.cpp
            tests/unit/point_field_bucket.cpp
            tests/unit/point_field_component.cpp
            tests/unit/project_settings.cpp
            tests/unit/primitive_resolve.cpp
            tests/unit/camera.cpp
            tests/unit/camera_resolve.cpp
            tests/unit/control_movement.cpp
            tests/unit/constant_motion.cpp
            tests/unit/camera_rig.cpp
            tests/unit/game_mode.cpp
            tests/unit/viewport_mapping.cpp
            tests/unit/managed_viewports.cpp
            tests/unit/dynamic_resolution.cpp
            tests/unit/gui_draw_list.cpp
            tests/unit/gui_layout.cpp
            tests/unit/gui_style_states.cpp
            tests/unit/gui_input.cpp
            tests/unit/gui_surface_input.cpp
            tests/unit/gui_widgets.cpp
            tests/unit/gui_document_api.cpp
            tests/unit/gui_presence.cpp
            tests/unit/gui_placement.cpp
            tests/unit/gui_overlay_component.cpp
            tests/unit/capture_surface_component.cpp
            tests/unit/sim_clock.cpp
            tests/unit/transform_interpolation.cpp
            tests/unit/input_edge_latch.cpp
            tests/unit/net_transport.cpp
            tests/unit/net_connection.cpp
            tests/unit/net_lifecycle.cpp
    )
    target_link_libraries(veng_test_unit PRIVATE veng::veng doctest::doctest)

    # Some unit cases reach into engine internals under src/ (the backend
    # TypeMapping header below); that path is on the include search list so they
    # compile. The public reflection serializer is on the include/ tree. The tests/
    # dir carries support/TempPath.h (per-process scratch isolation).
    target_include_directories(veng_test_unit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/engine/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests)

    # The type-mapping (and barrier-decision, plan 04) cases include backend
    # headers (TypeMapping.h → Backend/Vulkan.h, which also pulls in GLFW + VMA),
    # so this target needs those *include dirs* — headers only, no run-time
    # driver dependency, so the pure-logic cases still run with no ICD. This is
    # allowed: include-hygiene guards *public* headers, not test use of backend
    # headers.
    target_link_libraries(veng_test_unit PRIVATE
            Vulkan::Vulkan
            glfw
            GPUOpen::VulkanMemoryAllocator
    )

    # Discover each TEST_CASE as its own ctest entry, labelled `unit` so
    # `ctest -L unit` is the fast, driver-free feedback loop.
    doctest_discover_tests(veng_test_unit PROPERTIES LABELS unit)

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
    add_executable(veng_test_death tests/death/death_main.cpp)
    target_link_libraries(veng_test_death PRIVATE veng::veng veng_test_support)
    # tovk_unmapped includes TypeMapping.h (→ Backend/Vulkan.h → GLFW + VMA);
    # headers only, like veng_test_unit.
    target_link_libraries(veng_test_death PRIVATE
            Vulkan::Vulkan
            glfw
            GPUOpen::VulkanMemoryAllocator
    )

    # Pure-logic death cases (no device; run with no ICD).
    add_test(NAME death.sentinel COMMAND veng_test_death sentinel)
    set_tests_properties(death.sentinel PROPERTIES LABELS death
            PASS_REGULAR_EXPRESSION "sentinel death case")

    add_test(NAME death.vertex_format_unknown COMMAND veng_test_death vertex_format_unknown)
    set_tests_properties(death.vertex_format_unknown PROPERTIES LABELS death
            PASS_REGULAR_EXPRESSION "Unknown vertex element Format")

    add_test(NAME death.tovk_unmapped COMMAND veng_test_death tovk_unmapped)
    set_tests_properties(death.tovk_unmapped PROPERTIES LABELS death
            PASS_REGULAR_EXPRESSION "ToVk\\(Format\\): unmapped format")

    add_test(NAME death.assert_message COMMAND veng_test_death assert_message)
    set_tests_properties(death.assert_message PROPERTIES LABELS death
            PASS_REGULAR_EXPRESSION "assert_message case fired")

    add_test(NAME death.scene_get_stale_entity COMMAND veng_test_death scene_get_stale_entity)
    set_tests_properties(death.scene_get_stale_entity PROPERTIES LABELS death
            PASS_REGULAR_EXPRESSION "dead or stale entity")

    add_test(NAME death.scene_get_missing_component COMMAND veng_test_death scene_get_missing_component)
    set_tests_properties(death.scene_get_missing_component PROPERTIES LABELS death
            PASS_REGULAR_EXPRESSION "lacks the component")

    add_test(NAME death.type_id_collision COMMAND veng_test_death type_id_collision)
    set_tests_properties(death.type_id_collision PROPERTIES LABELS death
            PASS_REGULAR_EXPRESSION "TypeId collision")

    add_test(NAME death.system_id_collision COMMAND veng_test_death system_id_collision)
    set_tests_properties(death.system_id_collision PROPERTIES LABELS death
            PASS_REGULAR_EXPRESSION "SystemId collision")

    add_test(NAME death.transform_parent_cycle COMMAND veng_test_death transform_parent_cycle)
    set_tests_properties(death.transform_parent_cycle PROPERTIES LABELS death
            PASS_REGULAR_EXPRESSION "parent is a descendant of child")

    add_test(NAME death.transform_parent_dead COMMAND veng_test_death transform_parent_dead)
    set_tests_properties(death.transform_parent_dead PROPERTIES LABELS death
            PASS_REGULAR_EXPRESSION "Hierarchy references a dead or stale entity")

    add_test(NAME death.spot_shadow_range_nonpositive COMMAND veng_test_death spot_shadow_range_nonpositive)
    set_tests_properties(death.spot_shadow_range_nonpositive PROPERTIES LABELS death
            PASS_REGULAR_EXPRESSION "ComputeSpotShadowView needs range > 0")

    add_test(NAME death.spot_shadow_cone_out_of_range COMMAND veng_test_death spot_shadow_cone_out_of_range)
    set_tests_properties(death.spot_shadow_cone_out_of_range PROPERTIES LABELS death
            PASS_REGULAR_EXPRESSION "ComputeSpotShadowView needs outerCone")

    add_test(NAME death.point_shadow_range_nonpositive COMMAND veng_test_death point_shadow_range_nonpositive)
    set_tests_properties(death.point_shadow_range_nonpositive PROPERTIES LABELS death
            PASS_REGULAR_EXPRESSION "ComputePointShadowView needs range > 0")

    add_test(NAME death.focus_double_pop COMMAND veng_test_death focus_double_pop)
    set_tests_properties(death.focus_double_pop PROPERTIES LABELS death
            PASS_REGULAR_EXPRESSION "mispaired or double pop")

    add_test(NAME death.shader_set_zero_reserved COMMAND veng_test_death shader_set_zero_reserved)
    set_tests_properties(death.shader_set_zero_reserved PROPERTIES LABELS death
            PASS_REGULAR_EXPRESSION "set 0 is reserved for the bindless registry")

    add_test(NAME death.shader_non_contiguous_set COMMAND veng_test_death shader_non_contiguous_set)
    set_tests_properties(death.shader_non_contiguous_set PROPERTIES LABELS death
            PASS_REGULAR_EXPRESSION "declared sets must be contiguous starting at 1")

    # GPU-coupled death cases (need a headless Context; skip with no driver).
    add_test(NAME death.buffer_upload_overrun COMMAND veng_test_death buffer_upload_overrun)
    set_tests_properties(death.buffer_upload_overrun PROPERTIES LABELS "gpu;death"
            SKIP_RETURN_CODE 77 PASS_REGULAR_EXPRESSION "upload out of range")

    add_test(NAME death.index_u16_into_u32 COMMAND veng_test_death index_u16_into_u32)
    set_tests_properties(death.index_u16_into_u32 PROPERTIES LABELS "gpu;death"
            SKIP_RETURN_CODE 77 PASS_REGULAR_EXPRESSION "cannot upload u16 indices")

    add_test(NAME death.index_u32_into_u16 COMMAND veng_test_death index_u32_into_u16)
    set_tests_properties(death.index_u32_into_u16 PROPERTIES LABELS "gpu;death"
            SKIP_RETURN_CODE 77 PASS_REGULAR_EXPRESSION "cannot upload u32 indices")

    add_test(NAME death.descriptor_type_mismatch COMMAND veng_test_death descriptor_type_mismatch)
    set_tests_properties(death.descriptor_type_mismatch PROPERTIES LABELS "gpu;death"
            SKIP_RETURN_CODE 77 PASS_REGULAR_EXPRESSION "not a sampled or storage image")

    # Include-hygiene guard: compiles every public header while linking only the
    # PUBLIC deps of veng (glm, fmt, nlohmann/json, ImGui). Because Vulkan/GLFW/VMA/nfd
    # link PRIVATE, this fails to build if a public header leaks a backend header.
    # Also links veng_editor::veng_editor so the sweep covers the VengEditor/
    # public headers: it propagates their include dir and the editor's PUBLIC deps
    # (glm/fmt/nlohmann/json/ImGui via veng) while keeping imnodes PRIVATE, so a
    # leaked backend include in an editor public header fails this build too.
    add_executable(veng_test_include_hygiene tests/include_hygiene.cpp)
    target_link_libraries(veng_test_include_hygiene PRIVATE veng::veng veng::graph
            veng_editor::veng_editor)

    add_test(NAME include_hygiene COMMAND veng_test_include_hygiene)

    # ---- MCP band (veng::mcp, driver-free) ----------------------------------
    # A sibling include-hygiene guard over the Veng/Mcp/ public headers, narrowed to
    # httplib-only: it links veng::mcp but NOT the vendored httplib directly, so a
    # httplib leak into a public header fails this build. nlohmann/json is not excluded
    # here — veng::mcp inherits it transitively through its PUBLIC veng::veng link (veng
    # itself carries nlohmann PUBLIC), so this test can no longer distinguish "an Mcp
    # header leaks nlohmann" from "nlohmann always rides along via veng::veng". A
    # different boundary than the Vulkan/GLFW exclusion above, so it gets its own test
    # rather than growing that link line.
    add_executable(veng_test_mcp_include_hygiene tests/mcp_include_hygiene.cpp)
    target_link_libraries(veng_test_mcp_include_hygiene PRIVATE veng::mcp)

    add_test(NAME mcp_include_hygiene COMMAND veng_test_mcp_include_hygiene)

    # The loopback proof: an McpServer on Port 0 + a `ping` tool, a background pump
    # loop, and a real HTTP initialize -> tools/list -> tools/call round-trip from a
    # client. It compiles the vendored httplib client into its own TU (exceptions on,
    # like the rest of the suite) + nlohmann/json, so it needs no new dependency. Pure
    # logic + loopback socket, no GPU, so it runs in the default band.
    add_executable(veng_test_mcp_loopback tests/mcp_loopback.cpp)
    target_link_libraries(veng_test_mcp_loopback PRIVATE veng::mcp nlohmann_json::nlohmann_json)
    target_compile_definitions(veng_test_mcp_loopback PRIVATE JSON_NOEXCEPTION)
    # The vendored httplib single-header is a SYSTEM include so clang-tidy skips the
    # third-party code it pulls into this TU, matching the imgui/stb vendor posture.
    target_include_directories(veng_test_mcp_loopback SYSTEM PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/mcp/src/Vendor)

    add_test(NAME mcp_loopback COMMAND veng_test_mcp_loopback)
    set_tests_properties(mcp_loopback PROPERTIES LABELS mcp)

    # The world-tools proof: a TypeRegistry with the builtins + a test component, a small
    # Scene, an McpHost returning it, and a loopback round-trip over world.list_entities /
    # entity.get / world.query / scene.stats — asserting the JSON, the ReflectToJson round-trip,
    # the unregistered-type isError, and pagination. Device-free, so it runs in the default band.
    add_executable(veng_test_mcp_world tests/mcp_world.cpp)
    target_link_libraries(veng_test_mcp_world PRIVATE veng::mcp veng::veng
            nlohmann_json::nlohmann_json)
    target_compile_definitions(veng_test_mcp_world PRIVATE JSON_NOEXCEPTION)
    target_include_directories(veng_test_mcp_world SYSTEM PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/mcp/src/Vendor)

    add_test(NAME mcp_world COMMAND veng_test_mcp_world)
    set_tests_properties(mcp_world PROPERTIES LABELS mcp)

    # The mutation-tools proof: an McpServer with AllowMutations on + a null ApplyMutation
    # (raw-scene path), a device-free Scene, and a loopback round-trip over entity.spawn /
    # add_component / get / set_field / remove_component / destroy — asserting each edit took, the
    # JsonToFields ↔ FieldsToJson round-trip, the stale-handle isError, and the spatial-version
    # bump, plus a read-only server proving the mutation tools are absent. Device-free (the
    # scene-only ops touch no Assets), so it runs in the default band.
    add_executable(veng_test_mcp_mutation tests/mcp_mutation.cpp)
    target_link_libraries(veng_test_mcp_mutation PRIVATE veng::mcp veng::veng
            nlohmann_json::nlohmann_json)
    target_compile_definitions(veng_test_mcp_mutation PRIVATE JSON_NOEXCEPTION)
    target_include_directories(veng_test_mcp_mutation SYSTEM PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/mcp/src/Vendor)

    add_test(NAME mcp_mutation COMMAND veng_test_mcp_mutation)
    set_tests_properties(mcp_mutation PROPERTIES LABELS mcp)

    # The input-injection proof: stand an McpServer up (AllowMutations, an InjectInput closure over a
    # headless Veng::Input), pump it on a background thread that rolls Input::BeginFrame each frame,
    # and drive input.send over loopback — key/mouse-button/move/scroll events land in the Input
    # snapshot, the batch shape-validation errors are whole-call, a rejected batch applies nothing,
    # a read-only server omits input.send, and a null-InjectInput host reports it unavailable.
    # Device-free (a null-window Input, no Assets), so it runs in the default band.
    add_executable(veng_test_mcp_input tests/mcp_input.cpp)
    target_link_libraries(veng_test_mcp_input PRIVATE veng::mcp veng::veng
            nlohmann_json::nlohmann_json)
    target_compile_definitions(veng_test_mcp_input PRIVATE JSON_NOEXCEPTION)
    target_include_directories(veng_test_mcp_input SYSTEM PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/mcp/src/Vendor)

    add_test(NAME mcp_input COMMAND veng_test_mcp_input)
    set_tests_properties(mcp_input PROPERTIES LABELS mcp)

    # The client proof: stand an McpServer up in-process (a ping tool + an always-failing
    # err tool), pump it on a background thread, and drive McpClient against it — ListTools,
    # CallTool ok, the tool-error shape (isError), the unknown-tool result, and a dead-port
    # connection error within the timeout. The client is exercised through its public header
    # (no httplib/nlohmann type); nlohmann here only parses the ping payload the test asserts
    # on. Device-free, so it runs in the default band.
    add_executable(veng_test_mcp_client tests/mcp_client.cpp)
    target_link_libraries(veng_test_mcp_client PRIVATE veng::mcp veng::veng
            nlohmann_json::nlohmann_json)
    target_compile_definitions(veng_test_mcp_client PRIVATE JSON_NOEXCEPTION)

    add_test(NAME mcp_client COMMAND veng_test_mcp_client)
    set_tests_properties(mcp_client PROPERTIES LABELS mcp)

    # The client-driver proof: stand an McpServer up in-process (ping / err / echo_args tools),
    # pump it on a background thread, and drive RunClientCli with argument vectors and captured
    # ostringstream sinks — the arg grammar, the payload-to-stdout / error-to-stderr / exit-code
    # map, --list/--search, key=value JSON typing, --json, and the label-prefixed error lines.
    # nlohmann here only asserts on the echoed payloads. Device-free, so it runs in the default band.
    add_executable(veng_test_mcp_cli tests/mcp_cli.cpp)
    target_link_libraries(veng_test_mcp_cli PRIVATE veng::mcp veng::veng
            nlohmann_json::nlohmann_json)
    target_compile_definitions(veng_test_mcp_cli PRIVATE JSON_NOEXCEPTION)

    add_test(NAME mcp_cli COMMAND veng_test_mcp_cli)
    set_tests_properties(mcp_cli PROPERTIES LABELS mcp)

    # The render-tools proof (mcp_screenshot) is registered after veng_test_shaders
    # (it renders the cube fixture through the test shader pack) further below.

    # ---- Editor unit band (device-free) -------------------------------------
    # The NodeGraph topology core and the rest of libveng_editor's pure logic.
    # veng_test_unit links only veng::veng, so the editor symbols need their own
    # device-free target: it links veng_editor::veng_editor (which links veng::veng
    # PUBLIC) + doctest, with editor/src on the include path so cases can reach the
    # editor's src-private headers. Runs under the same no-ICD band as veng_test_unit.
    add_executable(veng_test_editor_unit
            tests/unit/main.cpp
            tests/editor/node_graph.cpp
            tests/editor/node_catalog_serialize.cpp
            tests/editor/material_catalog_compile.cpp
            tests/editor/field_widget.cpp
            tests/editor/asset_source_index.cpp
            tests/editor/editor_camera.cpp
            tests/editor/preview_capability.cpp
            tests/editor/status_tracker.cpp
            tests/editor/editor_mcp.cpp
            ${VENG_EDITOR_MCP}
    )
    # The serialization cases drive the JSON object directly, so the test target
    # links nlohmann/json (PRIVATE in libveng_editor — never exposed in a header).
    # veng::mcp: the editor MCP tools (VENG_EDITOR_MCP, compiled here as it is into the exe
    # rather than the framework lib) register into an McpServer the editor_mcp case drives over
    # loopback; the mcp/src include reaches the shared ReflectToJson walk, and mcp/src/Vendor is a
    # SYSTEM include so clang-tidy skips the vendored httplib the loopback case pulls in.
    target_link_libraries(veng_test_editor_unit PRIVATE veng_editor::veng_editor veng::mcp
            doctest::doctest nlohmann_json::nlohmann_json)
    target_compile_definitions(veng_test_editor_unit PRIVATE JSON_NOEXCEPTION)
    target_include_directories(veng_test_editor_unit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/editor/src
            ${VENG_EDITOR_MCP_INCLUDE} ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_include_directories(veng_test_editor_unit SYSTEM PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/mcp/src/Vendor)

    doctest_discover_tests(veng_test_editor_unit PROPERTIES LABELS unit)

    # ---- Module ABI + loader (plan 01, driver-free) -------------------------
    # A real game module and a wrong-version variant, each a SHARED library
    # linking veng::veng and exporting VengModuleRegister + VengModuleAbiVersion.
    # bad_version_module forces a mismatched ABI version so the loader rejects it.
    # loader_test loads both through the real platform loader by the path CMake
    # bakes in, proving the load -> version-check -> register round-trip.
    add_library(veng_test_module SHARED tests/module/test_module.cpp)
    target_link_libraries(veng_test_module PRIVATE veng::veng)

    add_library(veng_test_bad_version_module SHARED tests/module/bad_version_module.cpp)
    target_link_libraries(veng_test_bad_version_module PRIVATE veng::veng)
    target_compile_definitions(veng_test_bad_version_module PRIVATE VENG_MODULE_ABI_VERSION=999999u)

    add_executable(veng_test_loader tests/module/loader_test.cpp)
    target_link_libraries(veng_test_loader PRIVATE veng::veng)
    add_dependencies(veng_test_loader veng_test_module veng_test_bad_version_module)
    target_compile_definitions(veng_test_loader PRIVATE
            VENG_TEST_MODULE_PATH="$<TARGET_FILE:veng_test_module>"
            VENG_BAD_VERSION_MODULE_PATH="$<TARGET_FILE:veng_test_bad_version_module>"
    )

    add_test(NAME loader_test COMMAND veng_test_loader)
    set_tests_properties(loader_test PROPERTIES LABELS module)

    # Fuzzy image comparator for the golden smoke test (smoke_golden, registered
    # after the examples). Links no veng/Vulkan — just stb_image, which decodes
    # both the PPM capture and the PNG golden — so it runs with or without a GPU.
    add_executable(veng_test_golden_compare tests/golden_compare.cpp)
    target_include_directories(veng_test_golden_compare PRIVATE ${stb_SOURCE_DIR})

    # Headless smoke test: brings up a windowless context, renders off-screen and
    # verifies downloaded pixels. Skips (SKIP_RETURN_CODE 77) with no Vulkan ICD.
    add_executable(veng_test_headless_smoke tests/headless_smoke.cpp)
    target_link_libraries(veng_test_headless_smoke PRIVATE veng::veng veng_test_support)

    add_test(NAME headless_smoke COMMAND veng_test_headless_smoke)
    set_tests_properties(headless_smoke PROPERTIES LABELS gpu SKIP_RETURN_CODE 77)

    # ASTC capability probe for smoke_golden. The migrated hello-triangle scene
    # cooks ASTC textures, so the golden is reproducible only on a device that
    # supports textureCompressionASTC_LDR; this binary exits 77 when the codec is
    # absent (or no ICD is present) so the golden check skips rather than fails.
    add_executable(veng_test_astc_probe tests/astc_probe.cpp)
    target_link_libraries(veng_test_astc_probe PRIVATE veng_test_support)

    # Compute dispatch test (plan 05): headless graphics -> compute -> graphics
    # render graph proving cmd.Dispatch and the storage-image barrier chain.
    # Skips (SKIP_RETURN_CODE 77) with no Vulkan ICD.
    # Cooks the GPU tests' Slang shaders into a small .vengpack. The tests mount
    # it and LoadSync<ShaderAsset> their modules exactly as an app would — the
    # engine has no runtime Slang compiler, so cooking is the only path to a
    # shader module. None of these shaders declare vertex inputs, so the pack
    # needs no REFERENCE for a vertex-layout id.
    veng_add_asset_pack(veng_test_shaders
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
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/volume_write.comp.slang
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/volume_sample.comp.slang
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/sh_eval.comp.slang
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/invert.comp.shader.json
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/fullscreen.vert.shader.json
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/sample.frag.shader.json
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/bindless_sample.frag.shader.json
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/indirect_instance.vert.shader.json
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/indirect_instance.frag.shader.json
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/indirect_cull.comp.shader.json
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/volume_write.comp.shader.json
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/volume_sample.comp.shader.json
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/sh_eval.comp.shader.json)

    add_executable(veng_test_compute_dispatch tests/compute_dispatch.cpp)
    target_link_libraries(veng_test_compute_dispatch PRIVATE veng::veng veng_test_support)
    add_dependencies(veng_test_compute_dispatch veng_test_shaders)
    target_compile_definitions(veng_test_compute_dispatch PRIVATE
            TEST_SHADER_PACK="${CMAKE_CURRENT_BINARY_DIR}/test_shaders.vengpack"
    )

    add_test(NAME compute_dispatch COMMAND veng_test_compute_dispatch)
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
    add_executable(veng_test_gpu
            tests/gpu/main.cpp
            tests/gpu/buffer_roundtrip.cpp
            tests/gpu/typed_buffer_roundtrip.cpp
            tests/gpu/image_clear_format.cpp
            tests/gpu/descriptor_write_paths.cpp
            tests/gpu/isolation.cpp
            tests/gpu/bindless_registry.cpp
            # The byte-address storage-buffer bindless input (set-0 g_Buffers[]) spike +
            # the SetStorageBufferHandle material path: registers a buffer beside a sampler
            # in set 0 and reads it from a fragment shader through a Sky-domain material.
            tests/gpu/bindless_storage_buffer.cpp
            tests/gpu/ring_buffered_material.cpp
            tests/gpu/timeline_semaphore.cpp
            tests/gpu/transfer_retire.cpp
            tests/gpu/async_upload.cpp
            tests/gpu/async_mesh_build.cpp
            tests/gpu/dynamic_mesh.cpp
            tests/gpu/async_build.cpp
            tests/gpu/transient_aliasing.cpp
            tests/gpu/indirect_draw.cpp
            tests/gpu/adopt.cpp
            tests/gpu/create_async.cpp
            tests/gpu/primitive_resolve.cpp
            tests/gpu/scene_renderer.cpp
            tests/gpu/debug_draw.cpp
            tests/gpu/point_field.cpp
            tests/gpu/viewport.cpp
            tests/gpu/viewport_documents.cpp
            tests/gpu/splitscreen.cpp
            tests/gpu/hi_z_pyramid.cpp
            tests/gpu/volume_texture_3d.cpp
            tests/gpu/occlusion_test.cpp
            tests/gpu/prefab_spawn.cpp
            tests/gpu/level_loader.cpp
            tests/gpu/sh_consistency.cpp
            tests/gpu/environment_ibl.cpp
            tests/gpu/gui_surface.cpp
            # capture_surface.cpp cooks a capture fixture pack and drives the CaptureSurface
            # component end to end: the entity's material samples the scene captured from the
            # entity (a backdrop out of the camera's direct view), the refresh policy (on-demand
            # settles then re-arms, every-frame never idles), and teardown unregistering the
            # capture from the drive-list on component/entity/scene destruction.
            tests/gpu/capture_surface.cpp
            # level_overlay.cpp drives a headless Application through Run(), opening/closing
            # LevelOverlay handles from OnUpdate and asserting the router/scene/pause state (it needs
            # a Context for the overlay's viewport, so it rides the gpu band though it pins no pixels).
            tests/gpu/level_overlay.cpp
            # simulation_drive_list.cpp drives a headless Application, registering scenes as
            # engine-ticked simulations and asserting the tick/pause/view/capture drive semantics (it
            # needs a Context for the Application, so it rides the gpu band though it pins no pixels).
            tests/gpu/simulation_drive_list.cpp
    )
    target_link_libraries(veng_test_gpu PRIVATE veng::veng veng_test_support doctest::doctest)
    # transfer_retire.cpp reaches through Native.h (raw vk::/VMA handles via the
    # public escape hatch) to drive RetireOnTransfer/ReleaseBuffer, so this target
    # needs the backend headers' *include dirs* (Vulkan + GLFW + VMA). Allowed:
    # include-hygiene guards public headers, not test use of the Native escape hatch.
    target_link_libraries(veng_test_gpu PRIVATE
            Vulkan::Vulkan
            glfw
            GPUOpen::VulkanMemoryAllocator
    )
    # sky_bake.cpp exercises the renderer-internal SkyCubemapBake directly (reading back the
    # baked cube faces for the seam-continuity oracle), so this target sees engine/src — the
    # private renderer headers, whose symbols the shared lib exports by default visibility.
    target_include_directories(veng_test_gpu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/engine/src)
    # bindless_registry.cpp and texture_loader.cpp draw with the test shaders
    # (fullscreen.vert + bindless_sample.frag), loaded by id from the cooked pack.
    add_dependencies(veng_test_gpu veng_test_shaders)
    target_compile_definitions(veng_test_gpu PRIVATE
            TEST_SHADER_PACK="${CMAKE_CURRENT_BINARY_DIR}/test_shaders.vengpack"
    )

    # texture_loader.cpp cooks a fixture pack in-process (plan 06), so it needs
    # the cooker library and the cooker fixtures' directory.
    target_sources(veng_test_gpu PRIVATE
            tests/gpu/texture_loader.cpp
            # font_loader.cpp cooks the font fixture pack in-process (msdf-atlas-gen),
            # LoadSyncs the Font, and asserts the atlas + metrics + ShapeRun contract.
            tests/gpu/font_loader.cpp
            # gui_golden.cpp cooks the font fixture pack, builds a hand-authored DrawList,
            # renders it through GuiScenePass over a solid scene output, and fuzzy-compares
            # the composite to the committed golden PNG (the Gui image floor). It loads the
            # golden via stb_image (whose implementation is already linked from veng::cook).
            tests/gpu/gui_golden.cpp
            # gui_hdr_target.cpp builds an imperative document with a >1.0 background, drives it
            # through the GuiScenePass render-to-texture sink into a persistent RGBA16Sfloat
            # Gui::RenderTarget, and samples that target's GetOutputHandle downstream — asserting
            # the emissive texel round-trips above 1.0 and the producer's barrier leaves it
            # shader-read before the sample. Uses the test shader pack's bindless sampler.
            tests/gpu/gui_hdr_target.cpp
            # ui_document_load.cpp cooks a UIDocument + StyleSheet + Font fixture pack, LoadSyncs
            # the document (pulling its stylesheet + font as dependencies), asserts the recipe tree
            # + resolved rule variants + resolved deps, and Instantiates two independent trees.
            tests/gpu/ui_document_load.cpp
            # ui_document_hud.cpp ties the whole HUD path together: cooks the UI fixture pack,
            # LoadSyncs + Instantiates the document with a font resolver, binds a reflected
            # view-model, attaches it to an Offscreen viewport, and asserts the composited output
            # tracks a changed bound value — plus the editor's cook-on-demand MountMemory hot-swap.
            tests/gpu/ui_document_hud.cpp
            # ui_document_repeat.cpp cooks a document whose classed child carries a count="4" with
            # ${n} substitution, LoadSyncs + Instantiates it, and asserts Document::FindAllByClass
            # resolves the unrolled pool in tree order with each element's substituted id + text.
            tests/gpu/ui_document_repeat.cpp
            # document_host.cpp cooks the UI fixture pack and drives the split document lifecycle:
            # DocumentHost's lazy instantiate / SetOnInstantiate / Recreate / SetContext-bind, and
            # DocumentLayer's attach -> present -> detach, re-attach on recreation, interactive flag.
            tests/gpu/document_host.cpp
            # document_texture.cpp drives an imperative document through DocumentTexture into its
            # owned HDR target: render-to-target, dirty-gate skip with a stable handle, and a
            # resolution-change re-allocation + re-render.
            tests/gpu/document_texture.cpp
            # gui_overlay.cpp cooks the UI HUD fixture pack and drives the GuiOverlay component the
            # Viewport discovers: the component-driven composite is byte-identical to the hand-wired
            # DocumentHost + DocumentLayer path, a runtime-removed component detaches, two seated
            # viewports each claim only their seat's overlay (unbound → the primary), and a failed
            # load surfaces null rather than aborting.
            tests/gpu/gui_overlay.cpp
            tests/gpu/mesh_loader.cpp
            tests/gpu/shader_loader.cpp
            tests/gpu/material_loader.cpp
            tests/gpu/vertex_layout_loader.cpp
            # The graph-sourced material layout guard: cooks a graph whose generated
            # MaterialParams mixes a vec4 param with uint handle slots and renders it,
            # pinning the large-alignment-first ordering (std140/scalar offset agreement).
            tests/gpu/graph_material_layout.cpp
            # The parent/instance split: a hand-built cooked MaterialInstance blob over the
            # brick parent, the pipeline-sharing invariant, and the runtime MID path.
            tests/gpu/material_instance.cpp
            # The authoring → cook → render proof: cooks a *.vmatinst.json over a parent
            # and asserts the override renders a different albedo than the parent default.
            tests/gpu/material_instance_override.cpp
            # The RTT-into-material proof: a producer viewport's output, sampled
            # through a consumer viewport's material, in registration order.
            # Cooks the brick g-buffer fixture, so it is gated with the rest.
            tests/gpu/viewport_feeds_viewport.cpp
            # The multi-viewport isolation proof: two viewports with different
            # cameras in one frame each render through their own view, guarding the
            # per-view slotting of the shared view-constants buffer.
            tests/gpu/multi_viewport_isolation.cpp
            # The sky cubemap bake: cooks an analytic Sky material, bakes it into the
            # renderer-internal radiance cube, and asserts twelve-edge seam continuity,
            # the per-direction analytic radiance, in-place re-bake, and the baked cube
            # round-tripping the skybox display path (baked ≈ direct through the renderer).
            tests/gpu/sky_bake.cpp
    )
    target_link_libraries(veng_test_gpu PRIVATE veng::cook)
    target_compile_definitions(veng_test_gpu PRIVATE
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
            # The gbuffer fixture's brick shaders `#include "Veng/surface.slang"`,
            # so the in-process cook adds the engine core shader dir to its search path.
            VENG_CORE_SHADER_DIR="${VENG_CORE_SHADER_DIR}"
            # gui_golden.cpp reads its committed golden PNG from the source tree.
            GUI_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden"
    )
    # gui_golden.cpp loads its golden PNG through stb_image.
    target_include_directories(veng_test_gpu PRIVATE ${stb_SOURCE_DIR})

    add_test(NAME veng_test_gpu COMMAND veng_test_gpu)
    set_tests_properties(veng_test_gpu PROPERTIES LABELS gpu SKIP_RETURN_CODE 77)

    # ---- MCP render-tools proof (GPU band) ---------------------------------
    # A headless Context + a Viewport rendering a one-cube scene, an McpHost returning it
    # for the primary name, and a loopback round-trip over render.list_viewports /
    # render.stats / render.screenshot — decoding the returned base64 PNG and asserting its
    # dimensions match the viewport extent (plus a coarse non-blank check). It renders the
    # cube through the test shader pack, so it depends on veng_test_shaders (defined above)
    # and skips (77) with no ICD like the rest of the gpu band.
    add_executable(veng_test_mcp_screenshot tests/mcp_screenshot.cpp)
    target_link_libraries(veng_test_mcp_screenshot PRIVATE veng::mcp veng::veng
            veng_test_support nlohmann_json::nlohmann_json)
    target_compile_definitions(veng_test_mcp_screenshot PRIVATE JSON_NOEXCEPTION)
    # The vendored httplib (client) and stb_image (PNG decode) are SYSTEM includes so
    # clang-tidy skips the third-party code they pull into this TU.
    target_include_directories(veng_test_mcp_screenshot SYSTEM PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/mcp/src/Vendor ${stb_SOURCE_DIR})
    add_dependencies(veng_test_mcp_screenshot veng_test_shaders)
    target_compile_definitions(veng_test_mcp_screenshot PRIVATE
            TEST_SHADER_PACK="${CMAKE_CURRENT_BINARY_DIR}/test_shaders.vengpack")

    add_test(NAME mcp_screenshot COMMAND veng_test_mcp_screenshot)
    set_tests_properties(mcp_screenshot PROPERTIES LABELS "gpu;mcp" SKIP_RETURN_CODE 77)

    # ---- MaterialPreview surface (editor src) ------------------------------
    # The one GPU target that links libveng_editor: it constructs a
    # VengEditor::MaterialPreview (which owns a SceneRenderer + a Ref<ImGuiTexture>),
    # so it needs a windowed Context and the ImGui Vulkan backend. It cooks the brick
    # g-buffer fixture in-process (veng::cook), so it is gated on the editor library. A
    # second viewport-style SceneRenderer is recorded alongside the preview, so the
    # two-renderer cross-graph handoff runs under the validation gate. Skips
    # (SKIP_RETURN_CODE 77) with no Vulkan ICD.
    if (TARGET veng_editor)
        add_executable(veng_test_material_preview tests/material_preview.cpp)
        target_link_libraries(veng_test_material_preview PRIVATE
                veng::veng veng_editor veng::cook veng_test_support)
        # The test includes the editor's src-private MaterialPreview.h.
        target_include_directories(veng_test_material_preview PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/editor/src)
        target_compile_definitions(veng_test_material_preview PRIVATE
                GPU_GBUFFER_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/assets"
                # The brick shaders `#include "Veng/surface.slang"`; the in-process cook
                # adds the engine core shader dir to its Slang search path.
                VENG_CORE_SHADER_DIR="${VENG_CORE_SHADER_DIR}")

        add_test(NAME veng_test_material_preview COMMAND veng_test_material_preview)
        set_tests_properties(veng_test_material_preview PROPERTIES LABELS gpu SKIP_RETURN_CODE 77)
    endif ()

    # ---- Multi-viewport ImGui interaction ----------------------------------
    # Drives the editor's real shape headless: two ImGui windows side by side, each
    # hosting an Offscreen Viewport that renders a distinctly-colored scene, then
    # ImGuiLayer::Render into the UI's offscreen image. It reads that composited
    # image back (veng::cook cooks the white-plane fixture in-process) and asserts
    # the two halves are different colors — the guard against viewport duplication.
    # Windowed (ImGui Vulkan backend), so it skips (SKIP_RETURN_CODE 77) with no ICD.
    add_executable(veng_test_multi_viewport_imgui tests/multi_viewport_imgui.cpp)
    target_link_libraries(veng_test_multi_viewport_imgui PRIVATE
            veng::veng veng::cook veng_test_support)
    target_compile_definitions(veng_test_multi_viewport_imgui PRIVATE
            GPU_GBUFFER_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/assets"
            # The white-plane fixture reuses the brick shaders, which
            # `#include "Veng/surface.slang"`; the in-process cook adds the engine
            # core shader dir to its Slang search path.
            VENG_CORE_SHADER_DIR="${VENG_CORE_SHADER_DIR}")

    add_test(NAME veng_test_multi_viewport_imgui COMMAND veng_test_multi_viewport_imgui)
    set_tests_properties(veng_test_multi_viewport_imgui PROPERTIES LABELS gpu SKIP_RETURN_CODE 77)
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
# The cooker suite is its own executable + label, separate from `veng_test_unit`'s
# `unit` band: it links veng::cook (which links veng::veng for module reflection)
# and the module-reflection test loads the real libhello_triangle, so it is
# registered after the examples subdirectory defines that module. Like veng_test_unit
# it compiles with exceptions (doctest needs them); -fno-exceptions is PRIVATE to
# veng/cook.
if (VENG_BUILD_TESTS AND TARGET hello_triangle)
    add_executable(veng_test_cooker
            tests/unit/main.cpp
            tests/cooker/cook_roundtrip.cpp
            tests/cooker/texture_cook.cpp
            tests/cooker/compression_resolution.cpp
            tests/cooker/mesh_cook.cpp
            tests/cooker/shader_cook.cpp
            tests/cooker/material_cook.cpp
            tests/cooker/material_instance_cook.cpp
            tests/cooker/generate_id.cpp
            tests/cooker/graph_shader_cook.cpp
            tests/cooker/vertex_layout_cook.cpp
            tests/cooker/verify.cpp
            tests/cooker/module_reflect.cpp
            tests/cooker/prefab_cook.cpp
            tests/cooker/prefab_variant_cook.cpp
            tests/cooker/prefab_save_roundtrip.cpp
            tests/cooker/level_cook.cpp
            tests/cooker/input_map_cook.cpp
            tests/cooker/cook_source.cpp
            tests/cooker/ui_document_cook.cpp
            tests/cooker/stylesheet_cook.cpp
            tests/cooker/ui_document_repeat.cpp
    )
    # prefab_save_roundtrip.cpp uses VengEditor::PrefabSerialize, so the suite links
    # libveng_editor in addition to libveng_cook (no cycle: the cooker is never linked
    # by the editor framework). editor/src is on the include path for its headers.
    target_link_libraries(veng_test_cooker PRIVATE veng::cook veng_editor::veng_editor doctest::doctest)
    # engine/src is on the include path so the stylesheet cook test can reach the private
    # StyleSheetLoader.h decode entry point (DecodeStyleSheet), exercising the runtime variable
    # table end-to-end from a cooked blob; veng::cook already links veng::veng transitively.
    # cooker/src reaches the internal StyleParse.h so the color-value parser is unit-tested directly.
    target_include_directories(veng_test_cooker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/editor/src
            ${CMAKE_CURRENT_SOURCE_DIR}/engine/src
            ${CMAKE_CURRENT_SOURCE_DIR}/cooker/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_dependencies(veng_test_cooker hello_triangle veng_test_bad_version_module)
    target_compile_definitions(veng_test_cooker PRIVATE
            VENG_COOKER_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/cooker/fixtures"
            VENG_HELLO_TRIANGLE_MODULE_PATH="$<TARGET_FILE:hello_triangle>"
            VENG_BAD_VERSION_MODULE_PATH="$<TARGET_FILE:veng_test_bad_version_module>"
            VENG_HT_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/hello-triangle/assets"
            VENG_CORE_PACK_JSON="${VENG_CORE_PACK_JSON}"
            # A material cook reflects a fragment that `#include`s the engine header; the
            # cook adds the engine core shader dir to its Slang search path.
            VENG_CORE_SHADER_DIR="${VENG_CORE_SHADER_DIR}"
    )

    doctest_discover_tests(veng_test_cooker PROPERTIES LABELS cooker)
endif ()

if (VENG_BUILD_TESTS AND TARGET hello_triangle-launcher AND TARGET veng_test_golden_compare)
    add_test(NAME smoke_golden
        COMMAND ${CMAKE_COMMAND}
            -D VENG_SMOKE_BIN=$<TARGET_FILE:hello_triangle-launcher>
            -D VENG_COMPARE_BIN=$<TARGET_FILE:veng_test_golden_compare>
            -D VENG_PROBE_BIN=$<TARGET_FILE:veng_test_headless_smoke>
            -D VENG_ASTC_PROBE_BIN=$<TARGET_FILE:veng_test_astc_probe>
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

    # ---- MCP conformance smoke (gpu;mcp band) -------------------------------
    # The build-time analogue of the loopback tests, but through the shipping consumer path: it
    # launches hello_triangle-launcher under HT_MCP=0 (ephemeral port) + HT_SMOKE, reads the
    # launcher's stdout for the McpServer readiness line, then does initialize + tools/list over
    # loopback and asserts the engine tool families are present and world.list_entities returns
    # the sample scene. It compiles the vendored httplib client into its own TU (exceptions on,
    # like the rest of the suite) + nlohmann/json, so it needs no new dependency. Labelled gpu (it
    # drives the real render path); the harness returns 77 when the launcher itself skipped for
    # want of a Vulkan ICD, keeping it in the gpu skip band.
    add_executable(veng_test_mcp_conformance tests/mcp_conformance.cpp)
    target_link_libraries(veng_test_mcp_conformance PRIVATE nlohmann_json::nlohmann_json)
    target_compile_definitions(veng_test_mcp_conformance PRIVATE JSON_NOEXCEPTION
        VENG_LAUNCHER_BIN="$<TARGET_FILE:hello_triangle-launcher>")
    # The vendored httplib single-header is a SYSTEM include so clang-tidy skips the third-party
    # code it pulls into this TU, matching the imgui/stb vendor posture.
    target_include_directories(veng_test_mcp_conformance SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/mcp/src/Vendor)
    add_dependencies(veng_test_mcp_conformance hello_triangle-launcher)

    add_test(NAME mcp_conformance COMMAND veng_test_mcp_conformance)
    set_tests_properties(mcp_conformance PROPERTIES LABELS "gpu;mcp" SKIP_RETURN_CODE 77
        WORKING_DIRECTORY $<TARGET_FILE_DIR:hello_triangle-launcher>)

    # ---- Editor MCP conformance smoke (gpu;mcp band) -------------------------
    # The editor analogue of mcp_conformance, through the shipping veng-editor path: launches
    # veng-editor against the hello-triangle project with --mcp=0 + --mcp-write, then drives
    # render.stats and an editor.set_field Bloom toggle (a Configure recompile) against the
    # startup level document, asserting each call succeeds and the editor survives — the two
    # calls that crashed when the MCP host captured engine references before Run() created them.
    # The editor opens a real window, so a missing display skips like a missing ICD (77).
    if (TARGET veng-editor AND TARGET hello_triangle_editor)
        add_executable(veng_test_editor_mcp_conformance tests/editor_mcp_conformance.cpp)
        target_link_libraries(veng_test_editor_mcp_conformance PRIVATE nlohmann_json::nlohmann_json)
        target_compile_definitions(veng_test_editor_mcp_conformance PRIVATE JSON_NOEXCEPTION
            VENG_EDITOR_BIN="$<TARGET_FILE:veng-editor>"
            VENG_EDITOR_PROJECT="${CMAKE_CURRENT_SOURCE_DIR}/examples/hello-triangle/project.veng"
            VENG_EDITOR_BUILD_DIR="$<TARGET_FILE_DIR:hello_triangle-launcher>")
        target_include_directories(veng_test_editor_mcp_conformance SYSTEM PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/mcp/src/Vendor)
        # The editor dlopens the game + editor modules from the build dir and mounts the packs the
        # launcher's cook copied there, so the full example build must precede the test.
        add_dependencies(veng_test_editor_mcp_conformance
            veng-editor hello_triangle-launcher hello_triangle_editor)

        add_test(NAME editor_mcp_conformance COMMAND veng_test_editor_mcp_conformance)
        set_tests_properties(editor_mcp_conformance PROPERTIES LABELS "gpu;mcp" SKIP_RETURN_CODE 77
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    endif ()

    # ---- MCP client-CLI conformance smoke (gpu;mcp band) --------------------
    # The shipping-client analogue of mcp_conformance: it launches hello_triangle-launcher under
    # HT_MCP=0 + HT_SMOKE, reads the readiness line, then runs the same launcher as a --connect
    # client (--list, a render.stats + world.list_entities tools/call, and both nonzero error
    # paths) as a subprocess, asserting the client's own exit codes and payloads. It parses the
    # payloads with nlohmann/json but needs no httplib (it does not speak the wire itself — the
    # launcher's client does). Labelled gpu; the harness returns 77 when the launcher skipped for
    # want of a Vulkan ICD, keeping it in the gpu skip band.
    add_executable(veng_test_mcp_cli_conformance tests/mcp_cli_conformance.cpp)
    target_link_libraries(veng_test_mcp_cli_conformance PRIVATE nlohmann_json::nlohmann_json)
    target_compile_definitions(veng_test_mcp_cli_conformance PRIVATE JSON_NOEXCEPTION
        VENG_LAUNCHER_BIN="$<TARGET_FILE:hello_triangle-launcher>")
    add_dependencies(veng_test_mcp_cli_conformance hello_triangle-launcher)

    add_test(NAME mcp_cli_conformance COMMAND veng_test_mcp_cli_conformance)
    set_tests_properties(mcp_cli_conformance PROPERTIES LABELS "gpu;mcp" SKIP_RETURN_CODE 77
        WORKING_DIRECTORY $<TARGET_FILE_DIR:hello_triangle-launcher>)

    # ---- Editor MCP client-CLI conformance smoke (gpu;mcp band) -------------
    # The editor analogue of mcp_cli_conformance, through the shipping veng-editor path: launch
    # veng-editor --mcp against the hello-triangle project and drive it with veng-editor --connect
    # (--list, editor.list_panels, and both error paths). Exercises the editor exe's gated
    # --connect seam end to end. The editor opens a real window, so a missing display skips like a
    # missing ICD (77). Registered only when the editor exe exists and is built with MCP support.
    if (VENG_EDITOR_WITH_MCP AND TARGET veng-editor AND TARGET hello_triangle_editor)
        add_executable(veng_test_editor_mcp_cli_conformance tests/editor_mcp_cli_conformance.cpp)
        target_link_libraries(veng_test_editor_mcp_cli_conformance
            PRIVATE nlohmann_json::nlohmann_json)
        target_compile_definitions(veng_test_editor_mcp_cli_conformance PRIVATE JSON_NOEXCEPTION
            VENG_EDITOR_BIN="$<TARGET_FILE:veng-editor>"
            VENG_EDITOR_PROJECT="${CMAKE_CURRENT_SOURCE_DIR}/examples/hello-triangle/project.veng"
            VENG_EDITOR_BUILD_DIR="$<TARGET_FILE_DIR:hello_triangle-launcher>")
        add_dependencies(veng_test_editor_mcp_cli_conformance
            veng-editor hello_triangle-launcher hello_triangle_editor)

        add_test(NAME editor_mcp_cli_conformance COMMAND veng_test_editor_mcp_cli_conformance)
        set_tests_properties(editor_mcp_cli_conformance PROPERTIES LABELS "gpu;mcp"
            SKIP_RETURN_CODE 77 WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    endif ()

    # ---- Local validation-error gate ----------------------------------------
    # ctest -L validation: runs the gpu-labelled binaries that emit Vulkan
    # validation messages under VE_DEBUG and fails if a new [ERROR] Vulkan
    # validation line appears (see cmake/ValidationGate.cmake for the allowlist
    # of documented gaps). Includes the launcher path so the real shipping shape
    # runs under the gate too. Registered after the examples subdirectory so the
    # launcher target exists. Under the default (non-VE_DEBUG) build no validation
    # layers are enabled, so this is trivially green.
    # The MaterialPreview gate binary exists only when the editor is built; pass its
    # path through only when the target was built so the gate runs the two-renderer
    # handoff there too.
    set(VENG_GATE_MATERIAL_PREVIEW_ARG)
    if (TARGET veng_test_material_preview)
        set(VENG_GATE_MATERIAL_PREVIEW_ARG
            -D VENG_GATE_BIN_MATERIAL_PREVIEW=$<TARGET_FILE:veng_test_material_preview>)
    endif ()

    add_test(NAME validation_gate
        COMMAND ${CMAKE_COMMAND}
            -D VENG_GATE_BIN_HEADLESS_SMOKE=$<TARGET_FILE:veng_test_headless_smoke>
            -D VENG_GATE_BIN_COMPUTE_DISPATCH=$<TARGET_FILE:veng_test_compute_dispatch>
            -D VENG_GATE_BIN_GPU=$<TARGET_FILE:veng_test_gpu>
            -D VENG_GATE_BIN_LAUNCHER=$<TARGET_FILE:hello_triangle-launcher>
            ${VENG_GATE_MATERIAL_PREVIEW_ARG}
            -D VENG_GATE_LAUNCHER_CAPTURE=${CMAKE_CURRENT_BINARY_DIR}/validation_gate_launcher.ppm
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ValidationGate.cmake
    )
    set_tests_properties(validation_gate PROPERTIES LABELS validation)
endif ()

# Per-config cook isolation: asserts editing one build configuration re-cooks only its
# pack. Drives the real build graph (cook-all-packs) over hello-triangle's macOS/ASTC +
# Windows/BC7 configuration pair, so it is registered after the examples subdirectory
# defines those packs. ICD-free — a texture cook needs no GPU — so it runs unconditionally.
if (VENG_BUILD_TESTS AND TARGET hello_triangle_assets-macos
        AND TARGET hello_triangle_assets-windows)
    add_test(NAME cook_per_config_invalidation
        COMMAND ${CMAKE_COMMAND}
            -D VENG_BUILD_DIR=${CMAKE_BINARY_DIR}
            -D VENG_WINDOWS_CONFIG=${CMAKE_CURRENT_SOURCE_DIR}/examples/hello-triangle/configs/windows.buildcfg
            -D VENG_MACOS_PACK=$<TARGET_PROPERTY:hello_triangle_assets-macos,VENG_PACK_OUTPUTS>
            -D VENG_WINDOWS_PACK=$<TARGET_PROPERTY:hello_triangle_assets-windows,VENG_PACK_OUTPUTS>
            -D VENG_PREFAB_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/examples/hello-triangle/assets/prefabs/scene.prefab.json
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/PerConfigInvalidation.cmake
    )
    # Serialized: it touches a config and rebuilds the cooked packs, so it must not race
    # a test mounting the same build-tree pack. Disabled: the full re-cook it drives is
    # timing-sensitive and intermittently fails under concurrent test-suite load while
    # passing reliably in isolation, so it is skipped rather than flaking the gate.
    set_tests_properties(cook_per_config_invalidation PROPERTIES
        LABELS cooker RUN_SERIAL ON DISABLED ON)
endif ()

# SDK conformance: the out-of-tree find_package(veng) consumer proof. Each test discovers
# veng as a CMake package (an install prefix, or the build-tree export), then configures +
# builds the standalone examples/template against it and runs veng-editor --version — the
# build-time analogue of the relocatable-trio test, catching a broken install/export surface
# in ctest rather than a downstream first build. The in-tree consumption mode is covered
# separately by hello_triangle_launcher_smoke, so the template never builds in-tree.
#
# Registered only when veng builds the full SDK (the imported vengc / veng-editor the template
# resolves come from this build's install/export). Labelled gpu with SKIP_RETURN_CODE 77 — the
# driver's ICD probe returns 77 with no device, keeping them in the gpu skip band. Each carries
# a TIMEOUT so a hung inner build fails the test rather than stalling the whole run, and runs
# serially — each does a heavy nested configure + build.
if (VENG_BUILD_TESTS AND VENG_INSTALL_SDK AND TARGET veng-editor AND TARGET vengc
        AND TARGET veng_test_headless_smoke)
    foreach (MODE install buildtree)
        add_test(NAME sdk_conformance_${MODE}
            COMMAND ${CMAKE_COMMAND}
                -D VENG_MODE=${MODE}
                -D VENG_CMAKE=${CMAKE_COMMAND}
                -D VENG_ENGINE_BUILD_DIR=${CMAKE_BINARY_DIR}
                -D VENG_TEMPLATE_SRC=${CMAKE_CURRENT_SOURCE_DIR}/examples/template
                -D VENG_SCRATCH=${CMAKE_CURRENT_BINARY_DIR}/sdk_conformance_${MODE}
                -D VENG_PROBE_BIN=$<TARGET_FILE:veng_test_headless_smoke>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/SdkConformance.cmake
        )
        set_tests_properties(sdk_conformance_${MODE} PROPERTIES
            LABELS gpu SKIP_RETURN_CODE 77 RUN_SERIAL ON TIMEOUT 900)
    endforeach ()
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

# Engine core-data source: the core pack manifest (core.vengpack.json), the vertex-layout
# JSONs, and shaders/ (the Veng/surface.slang header a consumer shader #includes and the
# standard per-domain vertex shaders a material references by core-pack id). A downstream
# cook resolves core-pack ids from the source manifest (--reference) and #includes the
# engine shader header off its Slang search path — the installed source serves both, with
# no cooked artifact to keep in sync.
install(DIRECTORY engine/assets/core/ DESTINATION ${CMAKE_INSTALL_DATADIR}/veng/core)

# The authoring helper modules — the veng_add_project / veng_add_game /
# veng_add_asset_pack vocabulary a game drives its build with. They land beside the
# generated veng-config.cmake / veng-targets.cmake, which includes them so a
# find_package(veng) consumer gets the functions with no manual include of an SDK path.
install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/AssetPack.cmake
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Project.cmake
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Game.cmake
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Editor.cmake
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildConfig.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/veng
)

# The launcher source veng_add_game compiles into each game's <name>-launcher. Installed
# so VENG_LAUNCHER_MAIN resolves to a real file when the helpers load from a package.
install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/engine/src/Launcher/launcher_main.cpp
        DESTINATION ${CMAKE_INSTALL_DATADIR}/veng/launcher
)

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

# The shared config body both modes include() at config-load time — target import, alias
# recreation, and helper includes. It carries no @VAR@ substitution (the modes differ only
# in the preamble), so a plain configure_file lands one copy at the build-dir root beside the
# build-tree config; the same copy installs beside the install config.
configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/veng-config-body.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/veng-config-body.cmake
        @ONLY
)

write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/veng-config-version.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
)

# ---- Install-prefix config (the shipped SDK) --------------------------------
# Staged under a subdirectory, not the build-dir root, so it does not collide with the
# build-tree veng-config.cmake below; only the install(FILES) step consumes it.
configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/veng-config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/install-config/veng-config.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/veng
        PATH_VARS CMAKE_INSTALL_DATADIR
)

install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/install-config/veng-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/veng-config-version.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/veng-config-body.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/veng
)

# ---- Build-tree config (the co-development path, no install) -----------------
# export(EXPORT) writes veng-targets.cmake into the build-dir root with the imported
# vengc / veng-editor / editor libraries pointing at their build-tree locations, and the
# build-tree veng-config.cmake resolves beside it — so a game configured with
# -Dveng_ROOT=/path/to/veng/build (or that dir on CMAKE_PREFIX_PATH) discovers veng with no
# install step. The engine's cmake --build refreshes the exported targets in place.
#
# veng's exported link interface names glm::glm and fmt::fmt (PUBLIC on veng) and
# libzstd_static (PUBLIC through the static assetpack), so those dependency targets must be
# in a build-tree export for veng-targets.cmake to import against. fmt and zstd already write
# their own build-tree target files (_deps/fmt-build/fmt-targets.cmake,
# _deps/zstd-build/zstdTargets.cmake); glm only install-exports, so export its set here.
# glfw / VMA / nfd are PRIVATE to the shared veng and never enter its interface, so they need
# no build-tree export.
export(EXPORT glm
        NAMESPACE glm::
        FILE ${CMAKE_CURRENT_BINARY_DIR}/deps/glm-targets.cmake
)
export(EXPORT vengTargets
        NAMESPACE veng::
        FILE ${CMAKE_CURRENT_BINARY_DIR}/veng-targets.cmake
)

# A plain configure_file, not configure_package_config_file: the build-tree config sits at the
# build-dir root (one level), so @PACKAGE_INIT@'s install-depth prefix math would resolve to an
# unrelated ancestor. The build-tree preamble substitutes source-tree absolute paths directly.
configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/veng-config-buildtree.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/veng-config.cmake
        @ONLY
)
