# ---------------------------------------------------------------------------
# libveng_mcp — the optional MCP server library
# ---------------------------------------------------------------------------
#
# An optional engine-tier library a consuming app (game or editor) links to expose
# its live systems to AI agents over the Model Context Protocol. It is NOT linked by
# libveng and adds no dependency to any existing consumer — a distinct veng::mcp
# target a consumer opts into, exactly as veng::graph is. Its public surface is
# JSON-library-free (a tool handler is Result<string>(string_view)); nlohmann/json
# and cpp-httplib are PRIVATE and never reach a public header.

add_library(veng_mcp SHARED
        src/McpServer.cpp
        src/Vendor/HttpLib.cpp
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
# TU (HttpLib.cpp) and McpServer.cpp, which owns the httplib::Server. Per-TU exception
# settings are a supported mix: the runtime unwinder is always present; -fno-exceptions
# only forbids throw/catch in that TU. Safety rests on containment — httplib catches any
# throw at its own dispatch boundary (set_exception_handler / its routing try/catch) and
# veng's own handler code never throws, so none unwinds into a -fno-exceptions frame. The
# target-wide PCH is built -fno-exceptions, so skip it for these -fexceptions TUs.
set_source_files_properties(src/Vendor/HttpLib.cpp src/McpServer.cpp PROPERTIES
        COMPILE_OPTIONS -fexceptions
        SKIP_PRECOMPILE_HEADERS ON)

# Auto-export all symbols on MSVC so consumers link against this shared library
# without complete VE_API annotation — mirrors macOS/Linux default visibility.
if (MSVC)
    set_target_properties(veng_mcp PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif ()

# nlohmann/json — JSON-RPC (de)serialization used only in .cpp (never a public header),
# so a PRIVATE dependency: it leaks into neither consumers nor libveng. JSON_NOEXCEPTION
# matches the -fno-exceptions contract (parse errors are checked, not thrown).
target_link_libraries(veng_mcp PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(veng_mcp PRIVATE JSON_NOEXCEPTION)

# httplib needs threads and (on some platforms) the socket libraries; find_package
# supplies the portable Threads::Threads target.
find_package(Threads REQUIRED)
target_link_libraries(veng_mcp PRIVATE Threads::Threads)
