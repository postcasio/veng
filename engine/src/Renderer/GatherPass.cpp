#include <Veng/Renderer/GatherPass.h>

#include <Veng/Assert.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/Sampler.h>

namespace Veng::Renderer
{
    namespace
    {
        // Core pack fullscreen vertex stage, shared with the composite and the scene blits.
        constexpr AssetId FullscreenVertId{0xF46DD3C6F2AE0628ULL};
        constexpr AssetId GatherFragId{0x657ADCF9558CA6B9ULL};

        // The assembly target is linear HDR, matching the scene output's format so a
        // window-covering placement copies bit-for-bit.
        constexpr Format AssemblyFormat = Format::RGBA16Sfloat;

        // Bindless placement-texture and sampler indices for the gather shader. Matches
        // gather.frag PushConstants byte-for-byte.
        struct GatherPushConstants
        {
            u32 PlacementTexture;
            u32 Sampler;
        };
    }

    struct GatherPass::Impl
    {
        Renderer::Context& Context;
        Ref<Sampler> Sampler;
        AssetHandle<Shader> GatherVS;
        AssetHandle<Shader> GatherFS;
        Ref<PipelineLayout> Layout;
        Ref<GraphicsPipeline> Pipeline;
        SamplerHandle SamplerHandle;

        // The owned linear-HDR assembly target, the composite's single source.
        Ref<Image> AssemblyImage;
        Ref<ImageView> AssemblyView;
        uvec2 Extent;

        // The placement list bound for the upcoming frames, with one bindless texture slot
        // per placement. Replaced wholesale by SetPlacements.
        vector<CompositePlacement> Placements;
        vector<TextureHandle> PlacementHandles;

        // Imported assembly target, re-declared on every Compile and bound per replay.
        ResourceId AssemblyId;

        void CreateAssemblyTarget(uvec2 extent)
        {
            Extent = extent;
            AssemblyImage = Image::Create(
                Context, {
                             .Name = "Gather Assembly Target",
                             .Extent = {extent.x, extent.y, 1},
                             .Format = AssemblyFormat,
                             .Usage = ImageUsage::ColorAttachment | ImageUsage::Sampled,
                         });
            AssemblyView = ImageView::Create(Context, {
                                                          .Name = "Gather Assembly View",
                                                          .Image = AssemblyImage,
                                                      });
        }

        void ReleasePlacementHandles()
        {
            BindlessRegistry& bindless = Context.GetBindlessRegistry();
            for (const TextureHandle handle : PlacementHandles)
            {
                if (handle.IsValid())
                {
                    bindless.Release(handle);
                }
            }
            PlacementHandles.clear();
        }
    };

    Unique<GatherPass> GatherPass::Create(const GatherPassInfo& info)
    {
        return Unique<GatherPass>(new GatherPass(info));
    }

    GatherPass::GatherPass(const GatherPassInfo& info)
        : m_Impl(CreateUnique<Impl>(Impl{.Context = info.Context}))
    {
        m_Impl->Sampler = Sampler::Create(
            info.Context, {
                              .Name = "Gather Placement Sampler",
                              // Nearest + ClampToEdge: a same-resolution window-covering
                              // placement copies texel-for-texel, so the assembled values
                              // are bit-identical to the source.
                              .MagFilter = Filter::Nearest,
                              .MinFilter = Filter::Nearest,
                              .AddressModeU = AddressMode::ClampToEdge,
                              .AddressModeV = AddressMode::ClampToEdge,
                              .AddressModeW = AddressMode::ClampToEdge,
                          });

        const AssetResult<AssetHandle<Shader>> vs = info.Assets.LoadSync<Shader>(FullscreenVertId);
        VE_ASSERT(vs.has_value(), "{}", vs.error().Detail);
        m_Impl->GatherVS = *vs;

        const AssetResult<AssetHandle<Shader>> fs = info.Assets.LoadSync<Shader>(GatherFragId);
        VE_ASSERT(fs.has_value(), "{}", fs.error().Detail);
        m_Impl->GatherFS = *fs;

        m_Impl->Layout = PipelineLayout::Create(
            info.Context,
            {
                .Name = "Gather Layout",
                .PushConstantRanges =
                    {
                        PushConstantRange::Of<GatherPushConstants>(ShaderStage::Fragment),
                    },
            });

        m_Impl->Pipeline = GraphicsPipeline::Create(
            info.Context,
            {
                .Name = "Gather Pipeline",
                .ColorAttachments = {{.Format = AssemblyFormat}},
                .PipelineLayout = m_Impl->Layout,
                .ShaderStages =
                    {
                        {.Stage = ShaderStage::Vertex, .Module = m_Impl->GatherVS.Get()->Module},
                        {.Stage = ShaderStage::Fragment, .Module = m_Impl->GatherFS.Get()->Module},
                    },
            });

        m_Impl->CreateAssemblyTarget(info.Extent);

        m_Impl->SamplerHandle = info.Context.GetBindlessRegistry().Register(m_Impl->Sampler);
    }

    GatherPass::~GatherPass()
    {
        m_Impl->ReleasePlacementHandles();
        m_Impl->Context.GetBindlessRegistry().Release(m_Impl->SamplerHandle);
    }

    void GatherPass::SetPlacements(std::span<const CompositePlacement> placements)
    {
        VE_ASSERT(placements.size() <= MaxPresented,
                  "GatherPass: {} placements exceeds MaxPresented ({})", placements.size(),
                  MaxPresented);

        // Re-register exactly one bindless texture slot per placement, so a K-placement frame
        // declares exactly K samples and an empty list declares none.
        m_Impl->ReleasePlacementHandles();

        BindlessRegistry& bindless = m_Impl->Context.GetBindlessRegistry();
        m_Impl->Placements.assign(placements.begin(), placements.end());
        m_Impl->PlacementHandles.reserve(placements.size());
        for (const CompositePlacement& placement : m_Impl->Placements)
        {
            VE_ASSERT(placement.Texture, "GatherPass placement requires a non-null texture");
            m_Impl->PlacementHandles.emplace_back(bindless.Register(placement.Texture));
        }
    }

    void GatherPass::Resize(uvec2 extent)
    {
        m_Impl->CreateAssemblyTarget(extent);
    }

    const Ref<ImageView>& GatherPass::GetOutput() const
    {
        return m_Impl->AssemblyView;
    }

    std::span<const CompositePlacement> GatherPass::GetPlacements() const
    {
        return m_Impl->Placements;
    }

    Unique<CompiledGraph> GatherPass::Compile(RenderGraph& graph)
    {
        m_Impl->AssemblyId = graph.Import("GatherAssembly");

        graph.AddPass("Gather")
            .Color({
                .Resource = m_Impl->AssemblyId,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            })
            .Execute(
                [this](PassContext& ctx)
                {
                    CommandBuffer& cmd = ctx.Cmd();
                    const BindlessRegistry& bindless = m_Impl->Context.GetBindlessRegistry();
                    cmd.BindPipeline(m_Impl->Pipeline);
                    bindless.Bind(cmd);

                    // Each placement: scissor + viewport to its region, then a fullscreen
                    // triangle whose v_UV runs 0..1 across the region and samples the whole
                    // source. The clear filled the uncovered area.
                    for (usize i = 0; i < m_Impl->Placements.size(); ++i)
                    {
                        const ViewportRegion& region = m_Impl->Placements[i].Region;
                        cmd.SetViewport(region.Offset, region.Extent);
                        cmd.SetScissor(region.Offset, region.Extent);
                        cmd.PushConstants(GatherPushConstants{
                            .PlacementTexture = m_Impl->PlacementHandles[i].Index,
                            .Sampler = m_Impl->SamplerHandle.Index,
                        });
                        cmd.DrawFullscreenTriangle();
                    }
                });

        return graph.Compile();
    }

    void GatherPass::Execute(CommandBuffer& cmd, CompiledGraph& graph) const
    {
        // The placement textures are sampled through bindless inside the graph, which the
        // graph cannot see; transition them to Sample out of graph, before BeginRendering.
        for (const CompositePlacement& placement : m_Impl->Placements)
        {
            cmd.PrepareForAccess(placement.Texture, AccessKind::Sample);
        }

        const RenderGraph::ImportBinding bindings[] = {
            {.Id = m_Impl->AssemblyId, .View = m_Impl->AssemblyView},
        };
        graph.Execute(cmd, bindings);
    }
}
