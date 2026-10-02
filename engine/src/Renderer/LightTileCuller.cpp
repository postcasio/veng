#include "LightTileCuller.h"

#include <cstring>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/SceneView.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    namespace
    {
        // The light-tile cull compute shader (depth + the view's lights → per-tile light masks).
        constexpr AssetId LightTileCullCompId{0xFC9B4A56B2048E5FULL};

        // The cull's push block, matching light_tile_cull.comp PushConstants.
        struct LightTileCullPush
        {
            u32 ViewConstantsIndex;
            u32 DepthTexture;
        };
    }

    Unique<LightTileCuller> LightTileCuller::Create(Context& context, AssetManager& assets)
    {
        return Unique<LightTileCuller>(new LightTileCuller(context, assets));
    }

    LightTileCuller::LightTileCuller(Context& context, AssetManager& assets)
        : m_Context(context), m_FramesInFlight(context.GetMaxFramesInFlight())
    {
        const AssetResult<AssetHandle<Veng::Shader>> cullCs =
            assets.LoadSync<Veng::Shader>(LightTileCullCompId);
        VE_ASSERT(cullCs.has_value(), "LightTileCuller: shader load failed: {}",
                  cullCs.error().Detail);

        m_SetLayout = DescriptorSetLayout::Create(m_Context,
                                                  {
                                                      .Name = "SceneRenderer Light Tile Set Layout",
                                                      .Bindings = {{
                                                          .Binding = 0,
                                                          .Type = DescriptorType::StorageBuffer,
                                                          .Count = 1,
                                                          .Stages = ShaderStage::Compute,
                                                      }},
                                                  });
        m_Layout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Light Tile Layout",
                           .DescriptorSetLayouts = {m_SetLayout},
                           .PushConstantRanges = {PushConstantRange::Of<LightTileCullPush>(
                               ShaderStage::Compute)},
                       });
        m_Pipeline = ComputePipeline::Create(
            m_Context, {
                           .Name = "SceneRenderer Light Tile Cull Pipeline",
                           .PipelineLayout = m_Layout,
                           .ShaderStage = {.Stage = ShaderStage::Compute,
                                           .Module = cullCs.value().Get()->Module},
                       });
    }

    LightTileCuller::~LightTileCuller()
    {
        m_Context.GetBindlessRegistry().Release(m_Handle);
    }

    void LightTileCuller::Recreate(const bool enabled, const uvec2 renderAllocExtent)
    {
        const uvec2 grid = enabled ? LightTileGrid(renderAllocExtent) : uvec2(0);
        if (grid == m_Grid && (m_Buffer != nullptr) == (grid.x * grid.y != 0))
        {
            return;
        }

        // The old buffer may still be read by a frame in flight; its slot release defers through
        // the same window, and a fresh set is built rather than the live one rewritten.
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_Handle);
        m_Handle = StorageBufferHandle{};
        m_Buffer.reset();
        m_Sets.clear();
        m_Grid = grid;
        m_LastTiles = uvec2(0);
        if (grid.x * grid.y == 0)
        {
            return;
        }

        // Each frame's set binds its own region alone, so the cull's write is scoped to the words
        // that frame owns rather than the whole ring a frame still in flight is reading; the
        // region stride is therefore a multiple of the storage-buffer offset alignment.
        const u64 gridBytes = static_cast<u64>(grid.x) * grid.y * sizeof(u32);
        const u64 minAlign =
            GetVkPhysicalDevice(m_Context).getProperties().limits.minStorageBufferOffsetAlignment;
        const u64 alignment = std::max<u64>(minAlign, sizeof(u32));
        const u64 regionBytes = (gridBytes + alignment - 1) / alignment * alignment;
        m_RegionWords = static_cast<u32>(regionBytes / sizeof(u32));
        m_Buffer = Buffer::Create(m_Context, {
                                                 .Name = "SceneRenderer Light Tile Masks",
                                                 .Size = regionBytes * m_FramesInFlight,
                                                 .Usage = BufferUsage::Storage,
                                                 .DeviceLocal = true,
                                             });
        m_Handle = bindless.Register(m_Buffer);

        m_Sets.reserve(m_FramesInFlight);
        for (u32 slot = 0; slot < m_FramesInFlight; ++slot)
        {
            const Ref<DescriptorSet> set =
                DescriptorSet::Create(m_Context, {
                                                     .Name = "SceneRenderer Light Tile Set",
                                                     .Layout = m_SetLayout,
                                                 });
            set->Write(0, m_Buffer, slot * regionBytes, gridBytes);
            m_Sets.push_back(set);
        }
    }

    ResourceId LightTileCuller::Import(RenderGraph& graph, const bool active)
    {
        // A recompiled topology has recorded no cull yet.
        m_MaskId = ResourceId{};
        m_LastTiles = uvec2(0);
        if (active)
        {
            VE_ASSERT(IsAllocated(), "LightTileCuller::Import: the cull is wired but no mask ring "
                                     "is allocated");
            m_MaskId = graph.ImportBuffer("SceneRenderer Light Tile Masks");
        }
        return m_MaskId;
    }

    void LightTileCuller::DeclareCull(RenderGraph& graph, const ResourceId depthId,
                                      const TextureHandle depthHandle)
    {
        // The StorageBufferWrite on the mask import orders this pass ahead of the lighting pass's
        // read through the graph-derived buffer barrier; the depth read takes its attachment →
        // shader-read transition the same way.
        RenderGraph::PassBuilder builder = graph.AddComputePass("Light Tile Cull");
        builder.Sample(depthId);
        builder.StorageBufferWrite(m_MaskId);

        builder.Execute(
            [this, depthHandle](PassContext& inner)
            {
                const auto* view = static_cast<const SceneView*>(inner.UserData());
                VE_ASSERT(view != nullptr, "Light tile cull pass: null SceneView");
                const uvec2 tiles = LightTileGrid(view->RenderExtent);
                m_LastRegion = m_Context.GetCurrentFrameInFlight();
                m_LastTiles = tiles;
                if (tiles.x * tiles.y == 0)
                {
                    return;
                }

                const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
                CommandBuffer& cmd = inner.Cmd();
                cmd.BindPipeline(m_Pipeline);
                registry.Bind(cmd, PipelineBindPoint::Compute);
                cmd.BindDescriptorSets(DescriptorSetBindInfo{
                    .Sets = {m_Sets[m_LastRegion]},
                    .FirstSet = 3, // sets 0-2 are the typed bindless registries
                    .PipelineBindPoint = PipelineBindPoint::Compute,
                });
                cmd.PushConstants(LightTileCullPush{
                    .ViewConstantsIndex = registry.GetCurrentViewConstantsIndex(),
                    .DepthTexture = depthHandle.Index,
                });
                cmd.Dispatch(tiles.x, tiles.y, 1);
            });
    }

    uvec4 LightTileCuller::ViewState(const bool active, const u32 frameSlot) const
    {
        if (!active || !IsAllocated())
        {
            return {LightTilesNone, 0u, 0u, 0u};
        }
        return {m_Handle.Index, m_Grid.x, frameSlot * m_RegionWords, 0u};
    }

    LightTileMasks LightTileCuller::Readback() const
    {
        LightTileMasks result;
        if (!IsAllocated() || !m_MaskId.IsValid() || m_LastTiles.x * m_LastTiles.y == 0)
        {
            return result;
        }

        const vector<u8> bytes = m_Buffer->Download();
        const usize regionWords = m_RegionWords;
        VE_ASSERT(bytes.size() >= (m_LastRegion + 1) * regionWords * sizeof(u32),
                  "LightTileCuller::Readback: the mask ring is smaller than its regions");

        result.Tiles = m_LastTiles;
        result.Masks.resize(static_cast<usize>(m_LastTiles.x) * m_LastTiles.y);
        const usize regionBase = m_LastRegion * regionWords;
        for (u32 y = 0; y < m_LastTiles.y; ++y)
        {
            const usize source = (regionBase + static_cast<usize>(y) * m_Grid.x) * sizeof(u32);
            std::memcpy(&result.Masks[static_cast<usize>(y) * m_LastTiles.x], bytes.data() + source,
                        m_LastTiles.x * sizeof(u32));
        }
        return result;
    }
}
