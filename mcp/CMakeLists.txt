# ---------------------------------------------------------------------------
# libveng_mcp — the optional MCP server library
# ---------------------------------------------------------------------------
#
# An optional engine-tier library a consuming app (game or editor) links to expose
# its live systems to AI agents over the Model Context Protocol. It is NOT linked by
# libveng and adds no dependency to any existing consumer beyond what veng::veng
# itself now carries — a distinct veng::mcp target a consumer opts into, exactly as
# veng::graph is. A tool handler is still Result<string>(string_view) — no nlohmann
# type appears in a Veng/Mcp/ public header — but nlohmann/json now reaches this
# library's consumers transitively through the PUBLIC veng::veng link below (veng
# itself carries nlohmann PUBLIC). cpp-httplib stays PRIVATE and never reaches a
# public header or a consumer.

add_library(veng_mcp SHARED
        src/McpServer.cpp
        src/McpClient.cpp
        src/McpClientCli.cpp
        src/ReflectToJson.cpp
        src/WorldTools.cpp
        src/RenderTools.cpp
        src/AudioTools.cpp
        src/ViewportCapture.cpp
        src/MutationTools.cpp
        src/InputTools.cpp
        src/ProfileTools.cpp
        src/Vendor/HttpLib.cpp
        src/Vendor/StbImageWrite.cpp
)
add_library(veng::mcp ALIAS veng_mcp)

target_include_directories(veng_mcp PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# veng::veng — the public headers name the house vocabulary (string/vector/function/
# u16) and Veng::Result. PUBLIC so a consumer resolves those includes through this link.
target_link_libraries(veng_mcp PUBLIC veng::veng)

# Same -fno-exceptions enforcement as libveng: asserts are fatal, recoverable
# operations return Veng::Result<T>. The vendored httplib TU below overrides this.
target_compile_options(veng_mcp PRIVATE -fno-exceptions)
target_compile_definitions(veng_mcp PRIVATE VE_BUILD_SHARED)

target_precompile_headers(veng_mcp PRIVATE <Veng/Veng.h>)

# cpp-httplib (vendored single header, src/Vendor/httplib.h) carries inline try/throw
# bodies, so every TU that includes it is compiled -fexceptions: the vendor aggregation
# TU (HttpLib.cpp), McpServer.cpp (the httplib::Server), and McpClient.cpp (the
# httplib::Client). Per-TU exception settings are a supported mix: the runtime unwinder
# is always present; -fno-exceptions only forbids throw/catch in that TU. On the server
# side, safety rests on containment — httplib catches any throw at its own dispatch
# boundary (set_exception_handler / its routing try/catch) and veng's own handler code
# never throws, so none unwinds into a -fno-exceptions frame. On the client side, veng is
# the CALLER of httplib and nlohmann, so McpClient.cpp instead contains its own throws:
# every nlohmann read is non-throwing, every dump uses the replace handler, and each
# public method's whole body is wrapped in a TU-local try/catch — so no exception crosses
# into the -fno-exceptions front-end. McpClientCli.cpp is the same CALLER position over
# nlohmann alone (it includes no httplib): every read is non-throwing, so nothing throws,
# and it is -fexceptions only because nlohmann's headers name throw internally. The
# target-wide PCH is built -fno-exceptions, so skip it for these -fexceptions TUs.
set_source_files_properties(src/Vendor/HttpLib.cpp src/McpServer.cpp src/McpClient.cpp
        src/McpClientCli.cpp PROPERTIES
        COMPILE_OPTIONS -fexceptions
        SKIP_PRECOMPILE_HEADERS ON)

# Auto-export all symbols on MSVC so consumers link against this shared library
# without complete VE_API annotation — mirrors macOS/Linux default visibility.
if (MSVC)
    set_target_properties(veng_mcp PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif ()

# nlohmann/json — JSON-RPC (de)serialization used directly in .cpp, so this library
# still names the target itself (PRIVATE: never a public header here) even though
# veng::veng below now carries it PUBLIC too. JSON_NOEXCEPTION matches the
# -fno-exceptions contract (parse errors are checked, not thrown).
target_link_libraries(veng_mcp PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(veng_mcp PRIVATE JSON_NOEXCEPTION)

# stb_image_write (header-only) is vendored into the dedicated src/Vendor/StbImageWrite.cpp
# TU so the render tools' PNG encode does not reach into libveng's private vendor symbols.
# The include dir is PRIVATE — the encode is confined to RenderTools.cpp and the vendor TU,
# never a public header. ${stb_SOURCE_DIR} is the FetchContent-populated stb checkout.
target_include_directories(veng_mcp PRIVATE ${stb_SOURCE_DIR})

# stb_image_write's implementation calls the deprecated sprintf, which the VE_DEBUG build's
# inherited warning-as-error (-Werror / MSVC /WX) would reject. Vendored code is not held to
# veng's warning bar, so the vendor TU demotes that one warning — scoped to this TU, leaving
# veng's own code strict. MSVC flags the deprecation as C4996, disabled here; other compilers
# take the -W form.
set_source_files_properties(src/Vendor/StbImageWrite.cpp PROPERTIES
        COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/wd4996,-Wno-error=deprecated-declarations>")

# httplib needs threads and (on some platforms) the socket libraries; find_package
# supplies the portable Threads::Threads target.
find_package(Threads REQUIRED)
target_link_libraries(veng_mcp PRIVATE Threads::Threads)

# veng_mcp is an optional SDK library an out-of-tree find_package(veng) consumer opts into
# (target_link_libraries(app veng::mcp)) and the installed veng-editor exe links, so it joins
# the vengTargets export set and installs its public Veng/Mcp/ headers beside libveng — exactly
# as veng::graph does. The installed lib finds libveng beside it through @loader_path (macOS) or
# $ORIGIN (Linux). The vendored httplib stays PRIVATE — never a Veng/Mcp/ public header. nlohmann/json
# is PRIVATE here too, but reaches a consumer transitively through the PUBLIC veng::veng link
# (veng-config's find_dependency(nlohmann_json) already covers it), so no extra find_dependency
# is needed in veng-config on veng::mcp's account.
if (VENG_INSTALL_SDK OR PROJECT_IS_TOP_LEVEL)
    if (APPLE)
        set_target_properties(veng_mcp PROPERTIES INSTALL_RPATH "@loader_path")
    elseif (NOT MSVC)
        set_target_properties(veng_mcp PROPERTIES INSTALL_RPATH "$ORIGIN")
    endif ()

    install(TARGETS veng_mcp
            EXPORT vengTargets
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(DIRECTORY include/Veng/Mcp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Veng)
endif ()
