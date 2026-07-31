#include "RenderTools.h"

#include "ViewportCapture.h"

#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpTool.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Renderer/BindlessRegistry.h>
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
        // render.screenshot — the tonemapped output of a viewport as a PNG image content block,
        // plus its pixel dimensions. A world's own GuiOverlay is driven onto the viewport's layer
        // stack, so it is part of that output; only the app's overlay sits outside it.
        {
            McpTool tool;
            tool.Name = "render.screenshot";
            tool.Description =
                "Captures a viewport's rendered output as a PNG image (tonemapped 8-bit), "
                "including any GuiOverlay a world drives into that viewport but not the app's "
                "own overlay. Optional 'viewport' names the viewport (default the primary). "
                "Returns an image content block; over the --connect CLI it requires --output "
                "<file> to write the PNG (an image is never printed to stdout).";
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

        // render.screenshot_window — the presented frame, the only surface carrying the app's
        // own UI overlay.
        {
            // The capture reads the context's presented-frame mirror, so it must be armed before a
            // frame that is to be captured ends. Arming at registration — which precedes the first
            // Pump, and so the listener thread that could carry a call — means the first call
            // already finds a mirrored frame.
            if (Renderer::Context* const context =
                    host.RenderContext ? host.RenderContext() : nullptr)
            {
                context->ArmPresentedFrameCapture();
            }

            McpTool tool;
            tool.Name = "render.screenshot_window";
            tool.Description =
                "Captures the presented frame as a PNG image — the finished composite of the "
                "scene and the UI overlay drawn over it, which is what an app's interface looks "
                "like on screen. render.screenshot captures a viewport instead, which carries the "
                "scene and any GuiOverlay a world drives into it, but not the app's own overlay "
                "composited over the frame. Returns an image content block; over the --connect "
                "CLI it requires --output <file> to write the PNG (an image is never printed to "
                "stdout). Unavailable headless (no swap chain, and no UI overlay to capture) and "
                "where the surface did not grant transfer-source usage on its swap chain images.";
            tool.InputSchemaJson = R"({"type":"object","properties":{}})";
            tool.ReturnsContentBlocks = true;
            tool.Handler = [&host](string_view) -> Result<string>
            {
                Renderer::Context* const context =
                    host.RenderContext ? host.RenderContext() : nullptr;
                if (context == nullptr)
                {
                    return std::unexpected(
                        string("presented-frame capture is unavailable: this host exposes no "
                               "render context"));
                }
                return CaptureSwapChainContentBlocks(*context);
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

        // render.bindless — how much of each arrayed binding is left. Every one has a fixed
        // capacity whose exhaustion is a fatal assert on an otherwise ordinary registration, and
        // nothing warns on the way down — a free list just gets shorter. Read across a consumer's
        // own open/close cycle it separates the two ways an array runs out: a count that returns to
        // where it started names simultaneous occupancy, one that steps down per cycle names a leak.
        {
            McpTool tool;
            tool.Name = "render.bindless";
            tool.Description =
                "Reports the bindless registry's arrayed bindings: free slots and total capacity "
                "for textures, samplers, storage images, storage buffers, and materials. Takes no "
                "arguments.";
            tool.InputSchemaJson = R"({"type":"object","properties":{}})";
            tool.Handler = [&host](string_view) -> Result<string>
            {
                using Registry = Renderer::BindlessRegistry;
                const Renderer::BindlessCapacity free =
                    host.Assets.GetContext().GetBindlessRegistry().GetFreeSlots();
                return Json{
                    {"textures", {{"free", free.Textures}, {"capacity", Registry::MaxTextures}}},
                    {"samplers", {{"free", free.Samplers}, {"capacity", Registry::MaxSamplers}}},
                    {"storage_images",
                     {{"free", free.StorageImages}, {"capacity", Registry::MaxStorageImages}}},
                    {"storage_buffers",
                     {{"free", free.StorageBuffers}, {"capacity", Registry::MaxStorageBuffers}}},
                    {"materials", {{"free", free.Materials}, {"capacity", Registry::MaxMaterials}}},
                }
                    .dump();
            };
            server.RegisterTool(std::move(tool));
        }
    }
}
