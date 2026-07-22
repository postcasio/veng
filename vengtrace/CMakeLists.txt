# vengtrace — the binary-capture -> Chrome Trace Event JSON converter.
#
# A standalone CLI, a sibling of vengc: it builds unconditionally from source (veng is the
# tools), joins the vengTargets export set so an installed SDK carries it, is recreated under its
# unqualified name by veng-config so $<TARGET_FILE:vengtrace> resolves in every consumption mode,
# and carries an INSTALL_RPATH like the other installed tools. The decoder and converter live in
# the executable's own sources — the tool is small enough to need no separate library, and it is
# written against the normative format specification (docs/trace-format.md), sharing no code with
# the engine's writer.

set(VENGTRACE_SOURCES
        tool/main.cpp
        src/TraceDecoder.cpp
        src/ChromeTraceConverter.cpp
        src/Cli.cpp)

add_executable(vengtrace ${VENGTRACE_SOURCES})
target_include_directories(vengtrace PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# veng::veng supplies the house vocabulary header, nlohmann/json (PUBLIC on veng), and fmt; the
# converter touches no GPU and no engine subsystem beyond those headers.
target_link_libraries(vengtrace PRIVATE veng::veng nlohmann_json::nlohmann_json fmt::fmt)
target_compile_definitions(vengtrace PRIVATE JSON_NOEXCEPTION)
target_compile_options(vengtrace PRIVATE -fno-exceptions)

# The installed vengtrace ships with the SDK as an imported executable in vengTargets. The
# build-tree auto-rpath does not survive an install, so set an explicit INSTALL_RPATH: the
# co-installed libveng resolves through @loader_path/../lib (macOS) or $ORIGIN/../lib (Linux).
if (APPLE)
    set_target_properties(vengtrace PROPERTIES
            INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
else ()
    set_target_properties(vengtrace PROPERTIES
            INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif ()

install(TARGETS vengtrace
        EXPORT vengTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
