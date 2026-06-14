# libveng_assetformat — shared archive + cooked-blob format.
#
# Vulkan-free, importer-free, engine-independent: the one source of truth for
# the AssetId, archive container, and cooked-blob layouts that both `engine`
# and `cooker` link against.

add_library(libveng_assetformat STATIC
        src/Archive.cpp
)
add_library(veng::assetformat ALIAS libveng_assetformat)

target_include_directories(libveng_assetformat PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(libveng_assetformat PRIVATE fmt::fmt)

target_compile_options(libveng_assetformat PRIVATE -fno-exceptions)

# veng links libveng_assetformat PUBLIC, so it must join veng's export set for
# install(EXPORT vengTargets ...) (top-level CMakeLists) to resolve.
install(TARGETS libveng_assetformat
        EXPORT vengTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/Veng DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
