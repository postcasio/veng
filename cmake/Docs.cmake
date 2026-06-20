# Doxygen API reference for veng's public surface.
#
# Adds a `docs` target that runs Doxygen over the public include trees
# (engine/assetpack/cooker/editor) plus README.md as the main page. The house
# style documents every public declaration with `///` Doxygen comments, so the
# generated reference is the canonical browsable view of that surface.
#
# Doxygen is optional: when it is not installed the target is simply absent, so a
# normal build is unaffected. Output lands in ${CMAKE_BINARY_DIR}/docs/html
# (gitignored). Build it explicitly with `cmake --build build --target docs`.

function(veng_add_docs)
    find_package(Doxygen QUIET)
    if (NOT DOXYGEN_FOUND)
        message(STATUS "Doxygen not found — the `docs` target is unavailable.")
        return()
    endif ()

    # Public headers only: the consumer-facing reference, not the Vulkan backend.
    set(DOXYGEN_PROJECT_BRIEF "A modern C++26 Vulkan rendering engine")
    set(DOXYGEN_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/docs)
    set(DOXYGEN_GENERATE_HTML YES)
    set(DOXYGEN_GENERATE_LATEX NO)
    set(DOXYGEN_RECURSIVE YES)

    # Teach Doxygen the std:: containers/smart pointers so it can resolve scopes
    # like std::hash<T> specializations the public headers document.
    set(DOXYGEN_BUILTIN_STL_SUPPORT YES)

    # `///` doc comments with @-tags, plus README.md as the landing page.
    set(DOXYGEN_JAVADOC_AUTOBRIEF NO)
    set(DOXYGEN_USE_MDFILE_AS_MAINPAGE ${CMAKE_SOURCE_DIR}/README.md)

    # Document the API contract, not every private helper; warn on a public
    # declaration that slipped through without a doc comment.
    set(DOXYGEN_EXTRACT_ALL NO)
    set(DOXYGEN_WARN_IF_UNDOCUMENTED YES)
    set(DOXYGEN_QUIET YES)

    # VE_API is a dllexport/visibility attribute that carries no documentation
    # value; expanding it to nothing keeps it out of every signature. The module
    # ABI macro likewise vanishes so the free-function parse stays clean.
    set(DOXYGEN_MACRO_EXPANSION YES)
    set(DOXYGEN_EXPAND_ONLY_PREDEF YES)
    set(DOXYGEN_PREDEFINED
            "VE_API="
            "VE_EXPORT_MODULE_ABI()=")

    # Exclude the backend-only headers: they expose vk::/VMA types and are not
    # part of the consumer-facing surface.
    set(DOXYGEN_EXCLUDE ${CMAKE_SOURCE_DIR}/engine/include/Veng/Renderer/Backend)

    doxygen_add_docs(docs
            ${CMAKE_SOURCE_DIR}/engine/include/Veng
            ${CMAKE_SOURCE_DIR}/assetpack/include/Veng
            ${CMAKE_SOURCE_DIR}/cooker/include/Veng
            ${CMAKE_SOURCE_DIR}/editor/include/VengEditor
            ${CMAKE_SOURCE_DIR}/README.md
            COMMENT "Generating veng API reference with Doxygen")
endfunction()
