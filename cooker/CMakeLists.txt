# libveng_cook + the vengc CLI (planset-5 plan 03).
#
# Offline importer/cooker: parses a JSON asset pack, dispatches each entry to
# a registered AssetImporter, and writes a .vengpack archive via
# assetformat's ArchiveWriter. nlohmann/json is the first cooker-only
# dependency; stb/assimp/Slang (06-09) join it later. The engine never links
# any of these, nor libveng_cook itself.

option(VENG_BUILD_TOOLS "Build the asset-cooking tools (vengc)" ${PROJECT_IS_TOP_LEVEL})

if (NOT VENG_BUILD_TOOLS)
    add_library(libveng_cook INTERFACE)
    add_library(veng::cook ALIAS libveng_cook)
    return()
endif ()

# nlohmann/json — pack + per-asset JSON parsing (cooker-only; the runtime
# reads the binary archive, never JSON).
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# assimp — mesh import (plan 07). The one heavy dependency; strictly cooker-only
# (behind VENG_BUILD_TOOLS), so a consumer of libveng never pays for it. Trim it
# hard: no tools/tests/exports, only the importers veng's fixtures use (OBJ +
# glTF), and a static lib so vengc stays self-contained.
set(ASSIMP_BUILD_TESTS OFF CACHE INTERNAL "")
set(ASSIMP_INSTALL OFF CACHE INTERNAL "")
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE INTERNAL "")
set(ASSIMP_BUILD_SAMPLES OFF CACHE INTERNAL "")
set(ASSIMP_NO_EXPORT ON CACHE INTERNAL "")
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE INTERNAL "")
set(ASSIMP_BUILD_OBJ_IMPORTER ON CACHE INTERNAL "")
set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE INTERNAL "")
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE INTERNAL "")
set(ASSIMP_INJECT_DEBUG_POSTFIX OFF CACHE INTERNAL "")
set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
FetchContent_Declare(
        assimp
        GIT_REPOSITORY https://github.com/assimp/assimp.git
        GIT_TAG v5.4.3
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(assimp)

# Slang — .slang compilation + offline reflection (plan 08), the
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

add_library(slang::slang SHARED IMPORTED)
set_target_properties(slang::slang PROPERTIES
        IMPORTED_LOCATION ${Slang_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES ${Slang_INCLUDE_DIR}
)

add_library(libveng_cook STATIC
        src/Cooker.cpp
        src/BuiltinImporters.cpp
        src/Importers/RawImporter.cpp
        src/Importers/TextureImporter.cpp
        src/Importers/MeshImporter.cpp
        src/Importers/ShaderImporter.cpp
        src/Importers/VertexLayoutImporter.cpp
        src/Importers/VertexLayoutSource.cpp
        src/Vendor/StbImage.cpp
)
add_library(veng::cook ALIAS libveng_cook)

target_include_directories(libveng_cook PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# stb_image (texture decode, plan 06) — implementation detail of
# src/Vendor/StbImage.cpp and src/Importers/TextureImporter.cpp.
target_include_directories(libveng_cook PRIVATE
        ${stb_SOURCE_DIR}
)

# JSON_NOEXCEPTION: nlohmann/json's templates appear in libveng_cook's public
# headers (Cook/Types.h), so every consumer TU must agree on this define
# (ODR) — PUBLIC, not PRIVATE. With it, JSON_THROW becomes std::abort(), which
# compiles under -fno-exceptions; CookPack/AssetImporter::Cook guard every
# typed JSON access with is_*() checks first, so the abort paths are
# unreachable in normal operation — the same "loud one-line fix on drift"
# house style as VE_ASSERT.
target_compile_definitions(libveng_cook PUBLIC JSON_NOEXCEPTION)

target_link_libraries(libveng_cook PUBLIC veng::assetformat nlohmann_json::nlohmann_json)
# assimp/Slang are implementation details of MeshImporter.cpp/ShaderImporter.cpp
# — PRIVATE so they stay off libveng_cook's public link/include interface (no
# assimp/Slang headers leak).
target_link_libraries(libveng_cook PRIVATE fmt::fmt assimp::assimp slang::slang)

target_compile_options(libveng_cook PRIVATE -fno-exceptions)

add_executable(vengc tool/main.cpp)
target_link_libraries(vengc PRIVATE veng::cook fmt::fmt)
target_compile_options(vengc PRIVATE -fno-exceptions)
