// Include-hygiene guard for the veng::mcp public surface, narrowed to httplib-only.
//
// This TU includes every Veng/Mcp/ public header and is compiled into a target
// that links veng::mcp but is NOT given the cpp-httplib include directory (it
// links PRIVATE to veng_mcp). If any public header regresses and pulls in the
// vendored httplib.h, this file fails to compile. nlohmann/json is not excluded
// here: veng::mcp's PUBLIC veng::veng link carries nlohmann/json PUBLIC, so it
// always reaches this target regardless of what a Veng/Mcp/ header does — this
// test cannot distinguish a leaked <nlohmann/json.hpp> include from that
// transitive edge, so it guards httplib only.
//
// This is a different boundary than tests/include_hygiene.cpp's Vulkan/GLFW
// exclusion, so it is its own test: a failure names which boundary broke.

#include <Veng/Mcp/McpClient.h>
#include <Veng/Mcp/McpClientInfo.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpServerInfo.h>
#include <Veng/Mcp/McpTool.h>

int main()
{
    return 0;
}
