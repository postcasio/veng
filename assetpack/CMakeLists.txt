# libveng_assetpack — shared archive + cooked-blob format.
#
# Vulkan-free, importer-free, engine-independent: the one source of truth for
# the AssetId, archive container, and cooked-blob layouts that both `engine`
# and `cooker` link against.

add_library(libveng_assetpack STATIC
        src/Archive.cpp
        src/CookedProject.cpp
)
add_library(veng::assetpack ALIAS libveng_assetpack)

target_include_directories(libveng_assetpack PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(libveng_assetpack PRIVATE fmt::fmt)

# zstd PUBLIC: the reader inflates a compressed blob on resolve, so the codec
# reaches every assetpack consumer (libveng included). All zstd calls live in
# Archive.cpp; the public Archive.h carries only a plain codec field, so no
# <zstd.h> leaks into the public surface.
target_link_libraries(libveng_assetpack PUBLIC libzstd_static)

target_compile_options(libveng_assetpack PRIVATE -fno-exceptions)

# veng links libveng_assetpack PUBLIC, so it must join veng's export set for
# install(EXPORT vengTargets ...) (top-level CMakeLists) to resolve.
install(TARGETS libveng_assetpack
        EXPORT vengTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/Veng DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
