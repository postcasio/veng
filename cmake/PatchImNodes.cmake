# Run as the imnodes FetchContent PATCH_COMMAND (cmake -P, working directory = the source tree).
#
# imnodes is compiled into libveng and called by its consumers, so its API must be exported, but
# unlike ImGui (IMGUI_API) it has no export macro. This gives it one: every function declared in
# the public namespace and the two structs with out-of-line constructors take IMNODES_API, which
# Veng/Vendor/ImGuiConfig.h defines (imnodes.h includes imgui.h, and so that config, first). The
# rewrite is keyed to the pinned commit's layout and asserts it found what it expects, so a bumped
# pin that moves the declarations fails here rather than silently exporting nothing.

set(header "${CMAKE_CURRENT_SOURCE_DIR}/imnodes.h")
file(READ "${header}" text)

if (text MATCHES "IMNODES_API")
    return()
endif ()

string(REPLACE "struct ImNodesIO\n" "struct IMNODES_API ImNodesIO\n" text "${text}")
string(REPLACE "    struct EmulateThreeButtonMouse\n" "    struct IMNODES_API EmulateThreeButtonMouse\n"
        text "${text}")
string(REPLACE "    struct LinkDetachWithModifierClick\n"
        "    struct IMNODES_API LinkDetachWithModifierClick\n" text "${text}")
string(REPLACE "    struct MultipleSelectModifier\n" "    struct IMNODES_API MultipleSelectModifier\n"
        text "${text}")
string(REPLACE "struct ImNodesStyle\n" "struct IMNODES_API ImNodesStyle\n" text "${text}")

# Function declarations sit at column zero inside `namespace IMNODES_NAMESPACE`, one per line.
string(FIND "${text}" "namespace IMNODES_NAMESPACE" ns)
if (ns EQUAL -1)
    message(FATAL_ERROR "PatchImNodes: no IMNODES_NAMESPACE block in ${header}")
endif ()
string(SUBSTRING "${text}" 0 ${ns} head)
string(SUBSTRING "${text}" ${ns} -1 body)
string(REGEX REPLACE "\n((const )?[A-Za-z_][A-Za-z_0-9]*[*&]? +[*&]?[A-Za-z_][A-Za-z_0-9]*\\()"
        "\nIMNODES_API \\1" body "${body}")

string(REGEX MATCHALL "\nIMNODES_API " exported "${body}")
list(LENGTH exported count)
if (count LESS 70)
    message(FATAL_ERROR "PatchImNodes: annotated ${count} declarations, expected the full API")
endif ()

set(fallback "#ifndef IMNODES_API\n#define IMNODES_API\n#endif\n")
string(REPLACE "#endif\n\n#ifndef IMNODES_NAMESPACE" "#endif\n\n${fallback}\n#ifndef IMNODES_NAMESPACE"
        head "${head}")
if (NOT head MATCHES "define IMNODES_API")
    message(FATAL_ERROR "PatchImNodes: could not place the IMNODES_API fallback in ${header}")
endif ()

file(WRITE "${header}" "${head}${body}")
