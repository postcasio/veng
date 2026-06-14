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

add_library(libveng_cook STATIC
        src/Cooker.cpp
        src/BuiltinImporters.cpp
        src/Importers/RawImporter.cpp
        src/Importers/TextureImporter.cpp
        src/Importers/MeshImporter.cpp
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
# assimp is an implementation detail of MeshImporter.cpp — PRIVATE so it stays
# off libveng_cook's public link/include interface (no assimp headers leak).
target_link_libraries(libveng_cook PRIVATE fmt::fmt assimp::assimp)

target_compile_options(libveng_cook PRIVATE -fno-exceptions)

add_executable(vengc tool/main.cpp)
target_link_libraries(vengc PRIVATE veng::cook fmt::fmt)
target_compile_options(vengc PRIVATE -fno-exceptions)
