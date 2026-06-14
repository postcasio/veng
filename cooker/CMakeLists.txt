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

add_library(libveng_cook STATIC
        src/Cooker.cpp
        src/BuiltinImporters.cpp
        src/Importers/RawImporter.cpp
)
add_library(veng::cook ALIAS libveng_cook)

target_include_directories(libveng_cook PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
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
target_link_libraries(libveng_cook PRIVATE fmt::fmt)

target_compile_options(libveng_cook PRIVATE -fno-exceptions)

add_executable(vengc tool/main.cpp)
target_link_libraries(vengc PRIVATE veng::cook fmt::fmt)
target_compile_options(vengc PRIVATE -fno-exceptions)
