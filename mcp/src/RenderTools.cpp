#include "RenderTools.h"

#include "ViewportCapture.h"

#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpTool.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/FormatInfo.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Viewport.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>

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

        /// @brief Default page size for render.bindless_slots when the caller omits `limit`.
        ///
        /// A cap on how much of a 1024-slot array one call dumps into an agent's context; the
        /// caller pages the tail through nextCursor.
        constexpr u32 DefaultSlotLimit = 200;

        /// @brief Parses the optional `limit` argument, clamped to the page cap.
        u32 ParseSlotLimit(const Json& args)
        {
            if (!args.is_object() || !args.contains("limit") || !args["limit"].is_number())
            {
                return DefaultSlotLimit;
            }
            const i64 requested = args["limit"].get<i64>();
            if (requested <= 0)
            {
                return DefaultSlotLimit;
            }
            return static_cast<u32>(std::min<i64>(requested, DefaultSlotLimit));
        }

        /// @brief Parses the opaque `cursor` (the resume slot index), defaulting to 0.
        u32 ParseSlotCursor(const Json& args)
        {
            if (!args.is_object() || !args.contains("cursor"))
            {
                return 0;
            }
            const Json& cursor = args["cursor"];
            if (cursor.is_number())
            {
                const i64 value = cursor.get<i64>();
                return value < 0 ? 0 : static_cast<u32>(value);
            }
            if (cursor.is_string())
            {
                const string text = cursor.get<string>();
                u64 value = 0;
                if (std::from_chars(text.data(), text.data() + text.size(), value).ec ==
                    std::errc{})
                {
                    return static_cast<u32>(value);
                }
            }
            return 0;
        }

        /// @brief Which slot states a call reports: the `state` argument, defaulting to occupied.
        ///
        /// Occupied is the default because "what is in there" is the question the tool exists for;
        /// a free slot carries no description, so listing them is only useful for reading the
        /// fragmentation of the free list itself.
        struct SlotFilter
        {
            /// @brief Whether an occupied slot is reported.
            bool Occupied = true;
            /// @brief Whether a slot inside its deferred-release window is reported.
            bool PendingRelease = true;
            /// @brief Whether a free slot is reported.
            bool Free = false;
        };

        /// @brief Parses the optional `state` filter, or nothing when it names no known state.
        optional<SlotFilter> ParseSlotFilter(const Json& args)
        {
            if (!args.is_object() || !args.contains("state") || !args["state"].is_string())
            {
                return SlotFilter{};
            }
            const string state = args["state"].get<string>();
            if (state == "occupied")
            {
                return SlotFilter{};
            }
            if (state == "free")
            {
                return SlotFilter{.Occupied = false, .PendingRelease = false, .Free = true};
            }
            if (state == "pending_release")
            {
                return SlotFilter{.Occupied = false, .PendingRelease = true, .Free = false};
            }
            if (state == "all")
            {
                return SlotFilter{.Occupied = true, .PendingRelease = true, .Free = true};
            }
            return std::nullopt;
        }

        /// @brief Whether @p filter admits a slot in @p state.
        bool Admits(const SlotFilter& filter, const Renderer::BindlessSlotState state)
        {
            switch (state)
            {
            case Renderer::BindlessSlotState::Free:
                return filter.Free;
            case Renderer::BindlessSlotState::Occupied:
                return filter.Occupied;
            case Renderer::BindlessSlotState::PendingRelease:
                return filter.PendingRelease;
            }
            return false;
        }

        /// @brief One slot as the tool reports it, with the fields its array does not describe left
        ///        out rather than reported as zeros.
        Json DescribeSlot(const Renderer::BindlessArray array, const Renderer::BindlessSlot& slot)
        {
            Json out{{"index", slot.Index}, {"state", Renderer::BindlessSlotStateName(slot.State)}};
            if (!slot.Name.empty())
            {
                out["name"] = slot.Name;
            }
            if (slot.ImageFormat != Renderer::Format::Undefined)
            {
                out["format"] = Renderer::FormatName(slot.ImageFormat);
                out["extent"] = {slot.Extent.x, slot.Extent.y, slot.Extent.z};
                out["mips"] = slot.MipLevels;
                out["layers"] = slot.ArrayLayers;
                out["bytes"] = slot.ImageBytes;
            }
            else if (slot.SizeBytes != 0)
            {
                // The storage-buffer array reports the buffer's size; the material table reports
                // its cached block's length. Both are the slot's own byte count, so one key.
                out["bytes"] = slot.SizeBytes;
            }
            else if (array == Renderer::BindlessArray::Materials &&
                     slot.State != Renderer::BindlessSlotState::Free)
            {
                // A registered material whose block is empty is a real state, not a missing read.
                out["bytes"] = 0;
            }
            return out;
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
                "Reports the bindless registry's seven arrayed bindings: free slots and total "
                "capacity for textures, volumes, cubes, samplers, storage images, storage buffers, "
                "and materials. This is the 'how much is left' read; render.bindless_slots is the "
                "'what is in there' one. Takes no arguments.";
            tool.InputSchemaJson = R"({"type":"object","properties":{}})";
            tool.Handler = [&host](string_view) -> Result<string>
            {
                using Registry = Renderer::BindlessRegistry;
                const Renderer::BindlessCapacity free =
                    host.Assets.GetContext().GetBindlessRegistry().GetFreeSlots();
                return Json{
                    {"textures", {{"free", free.Textures}, {"capacity", Registry::MaxTextures}}},
                    {"volumes", {{"free", free.Volumes}, {"capacity", Registry::MaxVolumes}}},
                    {"cubes", {{"free", free.Cubes}, {"capacity", Registry::MaxCubes}}},
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

        // render.bindless_slots — what one array actually holds. A free count says a build is at
        // 80 % of MaxTextures; only the occupants say whether it is holding what it needs or the
        // same atlas nine times, which is the difference between a budget to raise and a leak to
        // fix.
        {
            McpTool tool;
            tool.Name = "render.bindless_slots";
            tool.Description =
                "Lists the slots of one bindless array with what each holds — the per-slot "
                "companion to render.bindless's free/capacity summary, and the way to find what is "
                "occupying an array that is running out. 'array' names it: textures, volumes, "
                "cubes, samplers, storage_images, storage_buffers, materials. An image array's "
                "slot reports the view's debug name, its format, the image extent, the mips and "
                "layers the view exposes, and the tightly-packed bytes those subresources occupy "
                "(the codec's own footprint, so a compressed texture reports compressed bytes); a "
                "storage buffer reports its name and size; a sampler its name; a material its "
                "cached parameter block's length. Optional 'state' filters by slot state — "
                "'occupied' (the default), 'free', 'pending_release' (released but still inside "
                "its deferred-release window, which is why a free count can trail the unoccupied "
                "count), or 'all'. Paginated on { limit, cursor }; 'capacity', 'free' and "
                "'matched' report the whole array, not the page.";
            tool.InputSchemaJson = R"({"type":"object","properties":{"array":{"type":"string"},)"
                                   R"("state":{"type":"string"},"limit":{"type":"integer"},)"
                                   R"("cursor":{"type":"string"}},"required":["array"]})";
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                const Json args = Json::parse(argsJson, nullptr, false);
                if (!args.is_object() || !args.contains("array") || !args["array"].is_string())
                {
                    return std::unexpected(string{
                        "'array' is required and names one of: textures, volumes, cubes, samplers, "
                        "storage_images, storage_buffers, materials"});
                }
                const string arrayName = args["array"].get<string>();
                const optional<Renderer::BindlessArray> array =
                    Renderer::ParseBindlessArray(arrayName);
                if (!array)
                {
                    return std::unexpected(fmt::format(
                        "'{}' names no bindless array; expected one of: textures, volumes, cubes, "
                        "samplers, storage_images, storage_buffers, materials",
                        arrayName));
                }
                const optional<SlotFilter> filter = ParseSlotFilter(args);
                if (!filter)
                {
                    return std::unexpected(
                        string{"'state' must be one of: occupied, free, pending_release, all"});
                }

                const Renderer::BindlessRegistry& registry =
                    host.Assets.GetContext().GetBindlessRegistry();
                const vector<Renderer::BindlessSlot> slots = registry.DescribeSlots(*array);
                const u32 cursor = ParseSlotCursor(args);
                const u32 limit = ParseSlotLimit(args);

                // 'matched' counts the whole array so a page reports how much of the filtered set
                // it is; the walk therefore counts every slot and only pages the emitted ones.
                Json items = Json::array();
                u32 matched = 0;
                u32 nextCursor = 0;
                bool more = false;
                for (const Renderer::BindlessSlot& slot : slots)
                {
                    if (!Admits(*filter, slot.State))
                    {
                        continue;
                    }
                    ++matched;
                    if (slot.Index < cursor)
                    {
                        continue;
                    }
                    if (items.size() >= limit)
                    {
                        if (!more)
                        {
                            more = true;
                            nextCursor = slot.Index;
                        }
                        continue;
                    }
                    items.push_back(DescribeSlot(*array, slot));
                }

                const Renderer::BindlessCapacity free = registry.GetFreeSlots();
                const u32 freeCount = [&]
                {
                    switch (*array)
                    {
                    case Renderer::BindlessArray::Textures:
                        return free.Textures;
                    case Renderer::BindlessArray::Volumes:
                        return free.Volumes;
                    case Renderer::BindlessArray::Cubes:
                        return free.Cubes;
                    case Renderer::BindlessArray::Samplers:
                        return free.Samplers;
                    case Renderer::BindlessArray::StorageImages:
                        return free.StorageImages;
                    case Renderer::BindlessArray::StorageBuffers:
                        return free.StorageBuffers;
                    case Renderer::BindlessArray::Materials:
                        return free.Materials;
                    }
                    return 0u;
                }();

                Json out{{"array", Renderer::BindlessArrayName(*array)},
                         {"capacity", Renderer::BindlessRegistry::CapacityOf(*array)},
                         {"free", freeCount},
                         {"matched", matched},
                         {"slots", std::move(items)}};
                if (more)
                {
                    out["nextCursor"] = std::to_string(nextCursor);
                }
                return out.dump();
            };
            server.RegisterTool(std::move(tool));
        }
    }
}
