# libveng_cook + the vengc CLI.
#
# Offline importer/cooker: parses a JSON asset pack, dispatches each entry to
# a registered AssetImporter, and writes a .vengpack archive via
# assetpack's ArchiveWriter. stb, assimp, Slang, and xxHash are cooker-only — the
# engine never links any of them, nor libveng_cook itself. nlohmann/json is the one
# exception: veng now carries it PUBLIC (the reflection JSON walker), so
# veng_cook_objs' own PUBLIC link below is redundant with the transitive edge but is
# kept — the cooker's public Cook/Types.h headers name nlohmann::json directly.
#
# veng is the tools: the cooker builds unconditionally whenever veng is built from
# source. A library-only consumer uses find_package(veng) and gets the prebuilt
# vengc imported from vengTargets instead of building the cooker's heavy deps.

# The cooker fetches its heavy third-party deps (bc7enc_rdo, astc-encoder, assimp)
# inline below. Shield them from the debug build's -Werror, inherited here as a
# directory COMPILE_OPTIONS from the top level: vendored code is not held to veng's
# warning bar. The inherited options are restored before the cooker's own targets.
get_directory_property(VENG_COOK_SAVED_COMPILE_OPTIONS COMPILE_OPTIONS)
set_directory_properties(PROPERTIES COMPILE_OPTIONS "")

# xxHash — fast non-crypto hash for archive content hashes (xxh3-128). Consumed
# header-only (stb pattern): XXH_IMPLEMENTATION is defined in exactly one cooker
# TU (src/Vendor/XxHash.cpp); every other site includes the header for
# declarations only. The repo's own CMake project lives under cmake_unofficial/,
# so populating it adds no build targets — just the header search path below.
FetchContent_Declare(
        xxhash
        GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
        GIT_TAG v0.8.2
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(xxhash)

# pugixml — the XML parser behind the UIDocumentImporter's `*.vui.xml` markup parse, cooker-only
# (never linked into libveng, which loads the binary element tree and parses no XML). A small,
# self-contained library with no transitive deps; built EXCLUDE_FROM_ALL as a static lib so only
# the parts the cooker links compile, and with exceptions off to match veng_cook_objs.
set(PUGIXML_NO_EXCEPTIONS ON CACHE INTERNAL "")
set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
FetchContent_Declare(
        pugixml
        GIT_REPOSITORY https://github.com/zeux/pugixml.git
        GIT_TAG v1.14
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(pugixml)

# pugixml inherits the tree's CXX_STANDARD 26, which AppleClang cannot express as a compiler flag
# (the same quirk the tree pins elsewhere). Pin its target back to C++17 — pugixml needs nothing
# newer — so its configure does not demand an unavailable dialect flag.
if (TARGET pugixml-static)
    set_target_properties(pugixml-static PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED OFF
            CXX_EXTENSIONS OFF)
endif ()

# bc7enc_rdo — BC7 block encoder for the TextureImporter, cooker-only (like
# stb/assimp/Slang; never linked into libveng, which uploads the cooked blocks and
# never encodes). The repo's CMakeLists builds a standalone
# app, so SOURCE_SUBDIR points at a directory with no CMakeLists — the populate adds
# the sources without any of its targets (the stb header-only pattern). bc7enc.cpp /
# bc7enc.h are a self-contained pair (libc + <algorithm> only); the encoder TU is
# compiled directly into veng_cook_objs below. Pinned to a concrete commit (the repo
# carries no release tags).
FetchContent_Declare(
        bc7enc_rdo
        GIT_REPOSITORY https://github.com/richgel999/bc7enc_rdo.git
        GIT_TAG dbe416d28a5530b4e8cc45b14bf034dc6b96bbde
        SOURCE_SUBDIR cmake
)
FetchContent_MakeAvailable(bc7enc_rdo)

# astc-encoder (ARM) — ASTC LDR block encoder for the TextureImporter, cooker-only (never
# linked into libveng, which uploads the cooked blocks and never encodes).
# A proper CMake project: build one per-architecture SIMD static codec library — NEON on Apple
# Silicon, SSE4.1 on x86_64 — with no CLI, no shared lib, no tests. Pinned to a tagged release.
#
# The SIMD codec is many times faster than the scalar "none" reference. ASTCENC_INVARIANCE keeps
# the encode bit-reproducible across runs, threads, and machines of the same architecture, so the
# cook stays deterministic — but cross-ISA output differs, so the smoke golden is regenerated when
# the codec ISA changes. astc-encoder's own universal (lipo) build is off: a single-arch SIMD lib
# matches veng's single-arch build (a universal veng build would need the scalar "none" codec).
if (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(ASTCENC_ISA_NEON ON CACHE INTERNAL "")
    set(VENG_ASTCENC_TARGET astcenc-neon-static)
else ()
    set(ASTCENC_ISA_SSE41 ON CACHE INTERNAL "")
    set(VENG_ASTCENC_TARGET astcenc-sse4.1-static)
endif ()
message(STATUS "veng: ASTC encoder ISA target = ${VENG_ASTCENC_TARGET}")
set(ASTCENC_ISA_NONE OFF CACHE INTERNAL "")
set(ASTCENC_ISA_NATIVE OFF CACHE INTERNAL "")
set(ASTCENC_UNIVERSAL_BUILD OFF CACHE INTERNAL "")
set(ASTCENC_CLI OFF CACHE INTERNAL "")
set(ASTCENC_SHAREDLIB OFF CACHE INTERNAL "")
set(ASTCENC_UNITTEST OFF CACHE INTERNAL "")
set(ASTCENC_INVARIANCE ON CACHE INTERNAL "")
FetchContent_Declare(
        astc_encoder
        GIT_REPOSITORY https://github.com/ARM-software/astc-encoder.git
        GIT_TAG 5.3.0
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(astc_encoder)

# FreeType — font-outline loader for the FontImporter's MSDF generation, cooker-only (never linked
# into libveng, which uploads the cooked atlas and decodes nothing). msdfgen finds it through the
# Freetype::Freetype target, so it is fetched here before msdf-atlas-gen below and its target
# aliased into the name msdfgen's find_package expects. Trim it to the core outline loader: no
# HarfBuzz, Brotli, PNG, zlib, or bzip2 subsystems, none of which the outline path needs. Built
# EXCLUDE_FROM_ALL so only the parts the cooker links compile.
set(FT_DISABLE_HARFBUZZ ON CACHE INTERNAL "")
set(FT_DISABLE_BROTLI ON CACHE INTERNAL "")
set(FT_DISABLE_PNG ON CACHE INTERNAL "")
set(FT_DISABLE_ZLIB ON CACHE INTERNAL "")
set(FT_DISABLE_BZIP2 ON CACHE INTERNAL "")
FetchContent_Declare(
        freetype
        GIT_REPOSITORY https://github.com/freetype/freetype.git
        GIT_TAG VER-2-13-3
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(freetype)
if (NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif ()

# msdf-atlas-gen / msdfgen — the multi-channel signed-distance-field glyph atlas + geometry
# generator behind the FontImporter, cooker-only (never linked into libveng, which decodes nothing
# from the cooked atlas). Only the msdfgen submodule is needed; the artery-font export is disabled,
# so its submodule is not fetched. Skia (a full renderer), vcpkg, PNG/SVG I/O, and the standalone
# executables are all disabled — the cook drives the library API directly and stores its own RGBA8
# atlas. The library uses the Freetype::Freetype target aliased above; it links no system package.
#
# On MSVC both libraries default to the static CRT (/MT[d]); veng and every other dependency use
# the dynamic CRT (/MD[d]), so their own DYNAMIC_RUNTIME opt-in is turned on to keep the runtime
# uniform — otherwise linking the cooker fails with LNK2038 RuntimeLibrary mismatches.
set(MSDFGEN_DYNAMIC_RUNTIME ON CACHE INTERNAL "")
set(MSDF_ATLAS_DYNAMIC_RUNTIME ON CACHE INTERNAL "")
set(MSDF_ATLAS_USE_VCPKG OFF CACHE INTERNAL "")
set(MSDF_ATLAS_USE_SKIA OFF CACHE INTERNAL "")
set(MSDF_ATLAS_BUILD_STANDALONE OFF CACHE INTERNAL "")
set(MSDF_ATLAS_NO_ARTERY_FONT ON CACHE INTERNAL "")
set(MSDF_ATLAS_INSTALL OFF CACHE INTERNAL "")
set(MSDFGEN_DISABLE_PNG ON CACHE INTERNAL "")
set(MSDFGEN_DISABLE_SVG ON CACHE INTERNAL "")
set(MSDFGEN_USE_SKIA OFF CACHE INTERNAL "")
set(MSDFGEN_USE_VCPKG OFF CACHE INTERNAL "")
set(MSDFGEN_BUILD_STANDALONE OFF CACHE INTERNAL "")
set(MSDFGEN_INSTALL OFF CACHE INTERNAL "")
FetchContent_Declare(
        msdf_atlas_gen
        GIT_REPOSITORY https://github.com/Chlumsky/msdf-atlas-gen.git
        GIT_TAG v1.3
        GIT_SHALLOW TRUE
        GIT_SUBMODULES msdfgen
        GIT_SUBMODULES_RECURSE TRUE
        EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(msdf_atlas_gen)

# The debug-optimize flag for the codec hot loops. MSVC's /O2 is rejected alongside the Debug
# runtime checks (/RTC1) the default Debug flags carry, so the flag is emitted only for compilers
# that accept optimization in a Debug build; under MSVC Debug the codecs stay unoptimized.
set(VENG_COOK_DEBUG_OPT $<$<AND:$<CONFIG:Debug>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-O2>)

# Optimize the ASTC codec even in a Debug build: it is a third-party hot loop nobody step-debugs,
# and an -O0 scalar/Debug encode dominates the cook wall-clock. Release keeps the codec's own -O3.
target_compile_options(${VENG_ASTCENC_TARGET} PRIVATE ${VENG_COOK_DEBUG_OPT})

# Same reasoning for the BC7/BC5/BC4 encoder TUs and the TextureImporter's mip-resize + block-encode
# loops: optimize in Debug so the cook does not run them unoptimized. Release inherits its own -O3.
set_source_files_properties(
        ${bc7enc_rdo_SOURCE_DIR}/bc7enc.cpp
        ${bc7enc_rdo_SOURCE_DIR}/rgbcx.cpp
        src/Importers/TextureImporter.cpp
        # The FontImporter drives the msdfgen MSDF render, another third-party hot loop the cook
        # runs at wall-clock cost; optimize it in Debug for the same reason.
        src/Importers/FontImporter.cpp
        PROPERTIES COMPILE_OPTIONS "${VENG_COOK_DEBUG_OPT}")

# nlohmann/json — pack + per-asset JSON parsing. veng's root CMakeLists.txt makes
# nlohmann_json::nlohmann_json available (now PUBLIC on veng itself) before this
# subdirectory is added, so no FetchContent of its own is needed here.

# assimp — mesh import. The one heavy dependency; strictly cooker-only, so a
# consumer of libveng never pays for it. Trim it
# hard: no tools/tests/exports, only the importers veng's assets use (OBJ +
# glTF + FBX, the rigged-character format), and a static lib so vengc stays
# self-contained.
set(ASSIMP_BUILD_TESTS OFF CACHE INTERNAL "")
set(ASSIMP_INSTALL OFF CACHE INTERNAL "")
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE INTERNAL "")
set(ASSIMP_BUILD_SAMPLES OFF CACHE INTERNAL "")
set(ASSIMP_NO_EXPORT ON CACHE INTERNAL "")
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE INTERNAL "")
set(ASSIMP_BUILD_OBJ_IMPORTER ON CACHE INTERNAL "")
set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE INTERNAL "")
set(ASSIMP_BUILD_FBX_IMPORTER ON CACHE INTERNAL "")
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE INTERNAL "")
set(ASSIMP_INJECT_DEBUG_POSTFIX OFF CACHE INTERNAL "")
set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
FetchContent_Declare(
        assimp
        GIT_REPOSITORY https://github.com/assimp/assimp.git
        GIT_TAG v5.4.3
        GIT_SHALLOW TRUE
)

# Hand assimp the zlib the top-level already built rather than letting it find a system
# copy (macOS/Linux) or compile its bundled contrib/zlib (Windows, where it defaults
# ASSIMP_BUILD_ZLIB=ON and the second `zlibstatic` target collides with ours). assimp
# keys its zlib choice on ZLIB_FOUND: ASSIMP_BUILD_ZLIB=ON skips its find_package, and a
# pre-set ZLIB_FOUND skips its bundled build, landing every platform on assimp's
# external-zlib path against our target — one zlib in the build, no collision, no
# system-vs-FetchContent divergence. block() keeps the ZLIB_* shims from leaking to
# sibling targets; ZLIB_INCLUDE_DIR spans the source (zlib.h) and binary (generated
# zconf.h) dirs.
block()
    set(ASSIMP_BUILD_ZLIB ON CACHE INTERNAL "")
    set(ZLIB_FOUND TRUE)
    set(ZLIB_LIBRARIES ZLIB::ZLIB)
    set(ZLIB_INCLUDE_DIR ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
    FetchContent_MakeAvailable(assimp)
endblock()

# Slang — .slang compilation + offline reflection, the
# ShaderImporter's implementation detail. Not fetched: the Slang SDK ships as
# part of the Vulkan SDK and is installed system-wide (slang/slang.h +
# libslang symlinked into /usr/local/include and /usr/local/lib on macOS).
# Install the Vulkan SDK (with its Slang component) to build vengc.
find_path(Slang_INCLUDE_DIR slang/slang.h)
find_library(Slang_LIBRARY slang)

if (NOT Slang_INCLUDE_DIR OR NOT Slang_LIBRARY)
    message(FATAL_ERROR
            "Slang not found (slang/slang.h / libslang). vengc's ShaderImporter "
            "needs the Slang SDK, which ships with the Vulkan SDK (its installer "
            "symlinks slang/slang.h and libslang into /usr/local/include and "
            "/usr/local/lib on macOS). Install/repair the Vulkan SDK, or point "
            "CMAKE_PREFIX_PATH at a Slang install.")
endif ()

# SHARED IMPORTED so $<TARGET_RUNTIME_DLLS> can copy slang.dll beside the tools that
# link it (vengc, the editor) — Windows has no rpath. A SHARED import on Windows needs
# the import lib (IMPORTED_IMPLIB) and the runtime DLL (IMPORTED_LOCATION) given
# separately; on macOS/Linux the single library file find_library found is both.
add_library(slang::slang SHARED IMPORTED)
if (WIN32)
    find_file(Slang_DLL NAMES slang.dll
            HINTS "${Slang_INCLUDE_DIR}/../Bin" "$ENV{VULKAN_SDK}/Bin"
            DOC "Path to slang.dll (Vulkan SDK Bin)")
    if (NOT Slang_DLL)
        message(FATAL_ERROR
                "slang.dll not found alongside the Slang SDK; vengc/the editor need it "
                "at runtime. Point CMAKE_PREFIX_PATH/VULKAN_SDK at the Vulkan SDK.")
    endif ()
    set_target_properties(slang::slang PROPERTIES
            IMPORTED_IMPLIB ${Slang_LIBRARY}
            IMPORTED_LOCATION ${Slang_DLL}
            INTERFACE_INCLUDE_DIRECTORIES ${Slang_INCLUDE_DIR})
else ()
    set_target_properties(slang::slang PROPERTIES
            IMPORTED_LOCATION ${Slang_LIBRARY}
            INTERFACE_INCLUDE_DIRECTORIES ${Slang_INCLUDE_DIR})
endif ()

# Restore the inherited warning options (the debug build's -Werror) for the
# cooker's own targets below — the vendored fetches above are now configured.
set_directory_properties(PROPERTIES COMPILE_OPTIONS "${VENG_COOK_SAVED_COMPILE_OPTIONS}")

# veng::cook_interface — the importer contract as headers only.
#
# A game's optional cook module registers real AssetImporters, so it needs AssetImporter,
# CookContext, Result, nlohmann-json, and assetpack — and nothing else. It must NOT link
# libveng_cook: that static library would carry a second copy of the cooker machinery and its
# process-wide state (the Slang session, the graph-shader resolver hook) into the dlopened image
# beside the tool's own. This INTERFACE target is that contract, consumed by both libveng_cook
# (through veng_cook_objs below) and by every out-of-tree cook module, so there is one contract
# and no duplicated machinery.
#
# It deliberately does not link veng::veng: veng_cook_objs must stay veng-free for the
# core-pack bootstrap. Cook/Importer.h names engine types (TypeRegistry, BuildConfiguration), so
# a consumer pairs this target with veng::veng — which a game cook module links anyway, to share
# its game lib's format headers.
add_library(veng_cook_interface INTERFACE)
add_library(veng::cook_interface ALIAS veng_cook_interface)
target_include_directories(veng_cook_interface INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
# JSON_NOEXCEPTION is an ODR-level agreement across every TU that sees nlohmann/json through
# the cooker's public headers, so it rides the interface rather than each consumer restating it.
target_compile_definitions(veng_cook_interface INTERFACE JSON_NOEXCEPTION)
target_link_libraries(veng_cook_interface INTERFACE
        nlohmann_json::nlohmann_json veng::assetpack)

install(TARGETS veng_cook_interface EXPORT vengTargets)
install(DIRECTORY include/Veng DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# The cooker's veng-free core. Everything except the module-reflection TU
# (src/ModuleTypes.cpp) lives here: importers, the Cooker, pack parsing, verify.
# It compiles against veng's public headers (Cook/Importer.h carries a
# TypeRegistry* field — decision 4's header-level dependency on the whole
# importer surface) but does NOT link veng, so the bootstrap cooker that cooks
# veng's own embedded core pack can use it without forming a build cycle
# (veng -> core-pack cook -> cooker -> veng). The object files compile once and
# are shared by both cooker executables below.
add_library(veng_cook_objs OBJECT
        src/AssetPack.cpp
        src/Cooker.cpp
        src/CookCache.cpp
        src/JsonFile.cpp
        src/Verify.cpp
        src/CoreImporters.cpp
        src/Importers/RawImporter.cpp
        src/Importers/TextureImporter.cpp
        src/Importers/MeshImporter.cpp
        src/Importers/ShaderImporter.cpp
        src/Importers/MaterialImporter.cpp
        src/Importers/MaterialInstanceImporter.cpp
        src/Importers/SlangReflect.cpp
        src/Importers/SlangSession.cpp
        src/Importers/GraphShaderHook.cpp
        src/Importers/VertexLayoutImporter.cpp
        src/Importers/VertexLayoutSource.cpp
        src/Importers/SkeletonImporter.cpp
        src/Importers/AnimationImporter.cpp
        src/Importers/SkeletonSource.cpp
        src/Importers/EnvironmentImporter.cpp
        src/Importers/FontImporter.cpp
        src/Vendor/StbImage.cpp
        src/Vendor/TinyExr.cpp
        src/Vendor/XxHash.cpp
        ${bc7enc_rdo_SOURCE_DIR}/bc7enc.cpp
        ${bc7enc_rdo_SOURCE_DIR}/rgbcx.cpp
)

# The importer contract (its include dir, nlohmann-json, assetpack, JSON_NOEXCEPTION) comes
# from the shared INTERFACE target above — one contract, two consumers.
target_link_libraries(veng_cook_objs PUBLIC veng_cook_interface)

# veng's public headers (Cook/Importer.h's TypeRegistry field) without linking
# veng — include dirs only, so no link cycle through the embedded core pack.
target_include_directories(veng_cook_objs PUBLIC
        $<TARGET_PROPERTY:veng::veng,INTERFACE_INCLUDE_DIRECTORIES>
)

# stb_image (texture decode) and xxHash (archive content hashing) — header-only
# implementation details of src/Vendor/StbImage.cpp / TextureImporter.cpp and
# src/Vendor/XxHash.cpp / Cooker.cpp & Verify.cpp respectively. tinyexr (HDR
# environment decode) and the zlib it compresses through back the EnvironmentImporter
# via src/Vendor/TinyExr.cpp.
target_include_directories(veng_cook_objs PRIVATE
        ${stb_SOURCE_DIR}
        ${xxhash_SOURCE_DIR}
        ${tinyexr_SOURCE_DIR}
        ${bc7enc_rdo_SOURCE_DIR}
        $<TARGET_PROPERTY:${VENG_ASTCENC_TARGET},INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:ZLIB::ZLIB,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:libzstd_static,INTERFACE_INCLUDE_DIRECTORIES>
)

# tinyexr defaults to exceptions; veng_cook_objs builds -fno-exceptions like the
# engine's TinyExr TU, so disable them for this vendor TU too.
set_source_files_properties(src/Vendor/TinyExr.cpp PROPERTIES
        COMPILE_DEFINITIONS TINYEXR_USE_EXCEPTIONS=0)

# Precompile the foundational header the importer TUs all parse. The vendor
# aggregation TUs #include third-party sources with their own feature macros —
# force-injecting Veng.h ahead of those is unsafe — and the debug-optimized
# encode TUs compile under ${VENG_COOK_DEBUG_OPT}, which clang rejects against
# a PCH built without it (__OPTIMIZE__ mismatch); both opt out.
target_precompile_headers(veng_cook_objs PRIVATE <Veng/Veng.h>)
set_source_files_properties(
        src/Vendor/StbImage.cpp
        src/Vendor/TinyExr.cpp
        src/Vendor/XxHash.cpp
        src/Importers/TextureImporter.cpp
        src/Importers/FontImporter.cpp
        ${bc7enc_rdo_SOURCE_DIR}/bc7enc.cpp
        ${bc7enc_rdo_SOURCE_DIR}/rgbcx.cpp
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# assimp/Slang headers are needed at compile time by MeshImporter/ShaderImporter
# (their link is on the executables below). msdf-atlas-gen (plus its msdfgen core/ext) headers back
# the FontImporter's MSDF generation (its link is on the executables below, alongside its FreeType
# transitive dep). msdfgen's headers guard an <msdfgen/msdfgen-config.h> include behind
# MSDFGEN_PUBLIC and gate the ext font loader on MSDFGEN_EXTENSIONS — both defined here so the
# include-dirs-only compile matches what a linking consumer of the static libraries would see (the
# config header is only emitted on an install build, which this cook-only fetch is not).
target_include_directories(veng_cook_objs PRIVATE
        $<TARGET_PROPERTY:assimp::assimp,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:slang::slang,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:msdf-atlas-gen::msdf-atlas-gen,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:msdfgen::msdfgen-core,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:msdfgen::msdfgen-ext,INTERFACE_INCLUDE_DIRECTORIES>
)
set_source_files_properties(src/Importers/FontImporter.cpp PROPERTIES
        COMPILE_DEFINITIONS "MSDFGEN_PUBLIC=;MSDFGEN_EXTENSIONS")

# JSON_NOEXCEPTION (carried by veng_cook_interface) makes JSON_THROW std::abort(), which
# compiles under -fno-exceptions; CookPack/AssetImporter::Cook guard every typed JSON access
# with is_*() checks first, so the abort paths are unreachable in normal operation — the same
# "loud one-line fix on drift" house style as VE_ASSERT.
target_compile_options(veng_cook_objs PRIVATE -fno-exceptions)
# veng_cook_objs pulls in fmt through veng's public headers but does not link
# fmt::fmt (only the executables below do), so it misses the PUBLIC /utf-8 fmt
# attaches to its consumers. fmt 12.x static_asserts on it under MSVC; add it
# here directly, matching the generator expression fmt uses.
target_compile_options(veng_cook_objs PRIVATE
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/utf-8>)

# libveng_cook — the full cooker library: the veng-free core plus the
# module-reflection TU (ModuleTypes.cpp), the prefab + level importers
# (PrefabImporter.cpp / LevelImporter.cpp), and the full-set registration
# aggregator (BuiltinImporters.cpp), linking veng::veng.
#
# These live here, not in veng_cook_objs, because they reuse libveng's reflection
# serializer (WriteFields): putting them in the veng-free objects would drag an
# unresolved libveng symbol into the veng_cook_bootstrap link. RegisterBuiltinImporters
# is here for the same reason — it references the prefab/level registrations. The
# bootstrap cooks no prefabs or levels, so it registers RegisterCoreImporters alone.
#
# veng::veng — the scoped cooker→libveng dependency inversion. Reflecting a
# module's native types means the cooker process loads libgame (which pulls
# libveng) and calls TypeRegistry/ModuleLoader symbols from libveng; the
# module-reflection path (Cook/ModuleTypes.h) needs those symbols, so the link
# is PUBLIC. The graphics stack comes along but is never initialized — no
# Context, no GLFW window — so the cooker stays headless; vengc already requires
# the Vulkan SDK for Slang, so this is build weight, not a new toolchain
# requirement. The clean cooker/runtime separation is relaxed only on this load
# path; ordinary importers (texture/mesh/shader/material) link nothing from
# libveng. veng's own embedded core pack is cooked by the veng-free bootstrap
# cooker below to keep this link off the core-pack build edge.
add_library(libveng_cook STATIC
        src/BuiltinImporters.cpp
        src/ModuleTypes.cpp
        # The cook-module loader reuses libveng's platform loader (LoadedModule), so it lives
        # here rather than in the veng-free core. The bootstrap loads no cook modules.
        src/CookModule.cpp
        src/Importers/PrefabImporter.cpp
        src/Importers/LevelImporter.cpp
        src/Importers/InputMapImporter.cpp
        # The UI-document + stylesheet importers reference libveng's Gui/ style vocabulary
        # (ElementKind/StyleProperty and the flex enums), so they live here (links veng::veng)
        # rather than in veng_cook_objs (kept veng-free for the core-pack bootstrap). pugixml (the
        # XML parse) and the CSS tokenizer are cooker-only, confined to these TUs.
        src/Importers/UIDocumentImporter.cpp
        src/Importers/StyleSheetImporter.cpp
        src/Importers/CssTokenizer.cpp
        src/Importers/StyleParse.cpp
        # The table importers compute their row layout from libveng's DataTable cell vocabulary,
        # so they live here rather than in veng_cook_objs (kept veng-free for the core-pack
        # bootstrap, which cooks no tables).
        src/Importers/TableImporters.cpp
        src/Importers/TableSchemaSource.cpp
        # The graph-sourced shader resolver runs the veng::graph emit walk, so it lives
        # here (links graph + veng) rather than in veng_cook_objs (kept veng-free for the
        # core-pack bootstrap). RegisterBuiltinImporters installs it as the resolver hook
        # the shader/material importers call; the bootstrap never installs it and cooks no
        # graph sources.
        src/Importers/GraphShaderSource.cpp)
add_library(veng::cook ALIAS libveng_cook)
target_link_libraries(libveng_cook PUBLIC veng_cook_objs veng::veng veng::assetpack veng::graph)
target_link_libraries(libveng_cook PRIVATE
        fmt::fmt assimp::assimp slang::slang ZLIB::ZLIB ${VENG_ASTCENC_TARGET}
        msdf-atlas-gen::msdf-atlas-gen pugixml::pugixml)
target_compile_options(libveng_cook PRIVATE -fno-exceptions)
target_precompile_headers(libveng_cook PRIVATE <Veng/Veng.h>)

add_executable(vengc tool/main.cpp)
target_link_libraries(vengc PRIVATE veng::cook fmt::fmt)

# The installed vengc ships with the SDK as an imported executable in vengTargets. The
# build-tree auto-rpath does not survive an install, so set an explicit INSTALL_RPATH: the
# co-installed libveng / libveng_graph resolve through @loader_path/../lib (macOS) or
# $ORIGIN/../lib (Linux), and the host's Slang resolves through the directory find_library
# found it in. Slang is not vendored — an installed veng requires the Vulkan SDK (with its
# Slang component) present to run the cooker, the same prerequisite the in-tree build carries.
get_filename_component(VENG_SLANG_LIBDIR "${Slang_LIBRARY}" DIRECTORY)
if (APPLE)
    set_target_properties(vengc PROPERTIES
            INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR};${VENG_SLANG_LIBDIR}")
else ()
    set_target_properties(vengc PROPERTIES
            INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR};${VENG_SLANG_LIBDIR}")
endif ()

install(TARGETS vengc
        EXPORT vengTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# veng_cook_bootstrap — a minimal cooker that links the veng-free core only (no
# veng::veng, no module reflection), used to cook veng's embedded core pack. It
# breaks the would-be cycle (veng -> core-pack cook -> cooker -> veng): the full
# vengc links veng, but this one does not, so veng can depend on it.
add_executable(veng_cook_bootstrap tool/bootstrap_main.cpp)
target_link_libraries(veng_cook_bootstrap PRIVATE
        veng_cook_objs veng::assetpack fmt::fmt assimp::assimp slang::slang ZLIB::ZLIB
        ${VENG_ASTCENC_TARGET} msdf-atlas-gen::msdf-atlas-gen)
target_compile_options(veng_cook_bootstrap PRIVATE -fno-exceptions)
target_compile_options(vengc PRIVATE -fno-exceptions)
