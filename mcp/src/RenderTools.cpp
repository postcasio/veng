#include "RenderTools.h"

#include "ViewportCapture.h"

#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpTool.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Viewport.h>

#include <nlohmann/json.hpp>

namespace Veng::Mcp
{
    using Json = nlohmann::json;

    namespace
    {
        /// @brief Reads the optional `viewport` name argument, defaulting to the empty (primary) name.
        string ViewportName(const Json& args)
        {
            if (args.is_object() && args.contains("viewport") && args["viewport"].is_string())
            {
                return args["viewport"].get<string>();
            }
            return {};
        }

        /// @brief Resolves a viewport by name through the host, or null when unset/unknown.
        Renderer::Viewport* ResolveViewport(const McpHost& host, const string& name)
        {
            return host.Viewport ? host.Viewport(name) : nullptr;
        }
    }

    void RegisterRenderTools(McpServer& server, const McpHost& host)
    {
        // render.screenshot — the tonemapped scene-color output of a viewport as a PNG image
        // content block, plus its pixel dimensions.
        {
            McpTool tool;
            tool.Name = "render.screenshot";
            tool.Description =
                "Captures a viewport's rendered output as a PNG image (tonemapped 8-bit "
                "scene color). Optional 'viewport' names the viewport (default the primary). "
                "The base64 image can be large (a full-window PNG); pass a smaller viewport to "
                "reduce it. Returns an image content block.";
            tool.InputSchemaJson =
                R"({"type":"object","properties":{"viewport":{"type":"string"}}})";
            tool.ReturnsContentBlocks = true;
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                const Json args = Json::parse(argsJson, nullptr, false);
                const string name = ViewportName(args);
                Renderer::Viewport* viewport = ResolveViewport(host, name);
                if (viewport == nullptr)
                {
                    return std::unexpected(name.empty()
                                               ? string("no primary viewport is available")
                                               : fmt::format("no viewport named '{}'", name));
                }

                return CaptureViewportContentBlocks(*viewport);
            };
            server.RegisterTool(std::move(tool));
        }

        // render.list_viewports — the viewports the host chose to expose, each with its region
        // extent and role where resolvable.
        {
            McpTool tool;
            tool.Name = "render.list_viewports";
            tool.Description = "Lists the viewports the host exposes: each name plus its region "
                               "extent and role (Presented/Offscreen) where resolvable.";
            tool.InputSchemaJson = R"({"type":"object","properties":{}})";
            tool.Handler = [&host](string_view) -> Result<string>
            {
                Json viewports = Json::array();
                const vector<string> names =
                    host.ViewportNames ? host.ViewportNames() : vector<string>{};
                for (const string& name : names)
                {
                    Json item{{"name", name}};
                    if (Renderer::Viewport* viewport = ResolveViewport(host, name))
                    {
                        const Renderer::ViewportRegion& region = viewport->GetRegion();
                        item["extent"] = Json::array({region.Extent.x, region.Extent.y});
                        item["role"] = viewport->GetRole() == Renderer::ViewportRole::Presented
                                           ? "Presented"
                                           : "Offscreen";
                    }
                    viewports.push_back(std::move(item));
                }
                return Json{{"viewports", std::move(viewports)}}.dump();
            };
            server.RegisterTool(std::move(tool));
        }

        // render.stats — a viewport's cull funnel and the completed-frame GPU time.
        {
            McpTool tool;
            tool.Name = "render.stats";
            tool.Description =
                "Reports a viewport's cull funnel (visible/frustum_survived/drawn/gpu_survivors, "
                "broadphase state) and the last completed-frame GPU time in milliseconds. Optional "
                "'viewport' names the viewport (default the primary).";
            tool.InputSchemaJson =
                R"({"type":"object","properties":{"viewport":{"type":"string"}}})";
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                const Json args = Json::parse(argsJson, nullptr, false);
                const string name = ViewportName(args);
                Renderer::Viewport* viewport = ResolveViewport(host, name);
                if (viewport == nullptr)
                {
                    return std::unexpected(name.empty()
                                               ? string("no primary viewport is available")
                                               : fmt::format("no viewport named '{}'", name));
                }

                const Renderer::SceneRenderer& renderer = viewport->GetRenderer();
                return Json{{"visible", renderer.GetLastVisibleCount()},
                            {"frustum_survived", renderer.GetFrustumSurvivedCount()},
                            {"drawn", renderer.GetLastDrawnCount()},
                            {"gpu_survivors", renderer.GetLastGpuSurvivorCount()},
                            {"broadphase_rebuilt", renderer.DidBroadphaseRebuildLastFrame()},
                            {"broadphase_nodes", renderer.GetBroadphaseNodeCount()},
                            {"gpu_frame_time_ms", host.Assets.GetContext().GetLastGpuFrameTimeMs()}}
                    .dump();
            };
            server.RegisterTool(std::move(tool));
        }
    }
}
