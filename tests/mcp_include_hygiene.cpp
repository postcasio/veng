// Include-hygiene guard for the veng::mcp public surface.
//
// This TU includes every Veng/Mcp/ public header and is compiled into a target
// that links veng::mcp but is NOT given the nlohmann/json or cpp-httplib include
// directories (both link PRIVATE to veng_mcp). If any public header regresses and
// pulls in <nlohmann/json.hpp> or the vendored httplib.h, this file fails to
// compile — keeping the JSON-library-free boundary from rotting.
//
// This is a different boundary than tests/include_hygiene.cpp's Vulkan/GLFW
// exclusion, so it is its own test: a failure names which boundary broke.

#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpServerInfo.h>
#include <Veng/Mcp/McpTool.h>

int main()
{
    return 0;
}
