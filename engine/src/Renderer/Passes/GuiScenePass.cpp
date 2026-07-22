#include "GuiScenePass.h"

#include <cmath>
#include <cstring>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Asset/VertexLayout.h>
#include <Veng/Gui/DrawList.h>
#include <Veng/Gui/RenderTarget.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Sampler.h>

namespace Veng::Renderer
{
    namespace
    {
        constexpr AssetId FullscreenVertId{0xF46DD3C6F2AE0628ULL};
        constexpr AssetId GatherFragId{0x657ADCF9558CA6B9ULL};
        constexpr AssetId GuiVertId{0x23896E307C8108E6ULL};
        constexpr AssetId GuiShapeFragId{0x65A3CA937C34B0C3ULL};
        constexpr AssetId GuiMsdfFragId{0x3F9BA093ED47FCEAULL};

        // Linear premultiplied-alpha "over": the fragments already output rgb premultiplied by
        // alpha, so the source color factor is One (not SrcAlpha) and both color and alpha
        // accumulate against OneMinusSrcAlpha.
        BlendState PremultipliedOver()
        {
            return {
                .Enable = true,
                .SrcColorFactor = BlendFactor::One,
                .DstColorFactor = BlendFactor::OneMinusSrcAlpha,
                .ColorOp = BlendOp::Add,
                .SrcAlphaFactor = BlendFactor::One,
                .DstAlphaFactor = BlendFactor::OneMinusSrcAlpha,
                .AlphaOp = BlendOp::Add,
            };
        }

        // Push block shared by the gui vertex and shape-fragment stages: the reciprocal of the UI
        // image extent (vertex, maps a framebuffer-pixel position to clip space) plus the gradient
        // record buffer's bindless slot and the current frame-in-flight's record base (fragment,
        // to load a vertex-selected gradient record) and the frame time an animated fill reads.
        struct GuiPushConstants
        {
            vec2 InvScreenSize;
            u32 GradientBuffer;
            u32 GradientBase;
            f32 Time;
        };

        // A GuiFill material's pipeline reserves this block verbatim and places its per-draw
        // selector immediately after it, so the domain's declared selector offset is this block's
        // size — the one place the two definitions can drift.
        static_assert(sizeof(GuiPushConstants) == GuiFillSelectorPushOffset,
                      "GuiPushConstants must be exactly the range a GuiFill material reserves "
                      "ahead of its selector (Veng::GuiFillSelectorPushOffset)");

        // Push block for the fullscreen composite/blit fragment (gather.frag): the bindless slots
        // of the sampled source and the sampler.
        struct BlitPushConstants
        {
            u32 Texture;
            u32 Sampler;
        };
    }

    struct GuiScenePass::Impl
    {
        Renderer::Context& Context;
        uvec2 Extent;
        // The draw-list-points → UI-image-pixels magnification (see SetUiScale); positions
        // scale through the vertex-stage clip transform, clip rects through the scissor.
        f32 UiScale = 1.0f;
        Format OutputFormat;

        // Resident shaders keep the modules alive for the pipelines' lifetime.
        AssetHandle<Veng::Shader> FullscreenVs;
        AssetHandle<Veng::Shader> GatherFs;
        AssetHandle<Veng::Shader> GuiVs;
        AssetHandle<Veng::Shader> ShapeFs;
        AssetHandle<Veng::Shader> MsdfFs;

        Ref<PipelineLayout> GuiLayout;
        Ref<GraphicsPipeline> ShapePipeline;
        Ref<GraphicsPipeline> MsdfPipeline;

        Ref<PipelineLayout> BlitLayout;
        Ref<GraphicsPipeline> ScenePipeline;   // opaque scene copy into the composite
        Ref<GraphicsPipeline> OverlayPipeline; // premultiplied-over UI blit into the composite

        Ref<Sampler> Sampler;
        SamplerHandle SamplerSlot;

        // The offscreen UI image (linear premultiplied alpha) and the composite target.
        Ref<Image> UiImage;
        Ref<ImageView> UiView;
        Ref<Image> CompositeImage;
        Ref<ImageView> CompositeView;

        // Ring-buffered geometry: one region per frame-in-flight, host-mapped for direct writes.
        Ref<Buffer> VertexBuffer;
        Ref<Buffer> IndexBuffer;
        u32 FramesInFlight = 0;
        u64 VertexRegionBytes = 0;
        u64 IndexRegionBytes = 0;

        // Ring-buffered gradient records, registered once into set-0 bindless as a byte-address
        // buffer the shape fragment loads from. GradientBase is this frame's record-index offset.
        Ref<Buffer> GradientBuffer;
        StorageBufferHandle GradientSlot;
        u64 GradientRegionBytes = 0;
        u32 GradientBase = 0;

        // The cached draw-list geometry bases + runs for the next Render, in the current region.
        // The whole ring is bound at offset 0, so each run's draw applies VertexBase as the index's
        // vertex offset and IndexBase as its first-index offset to reach this frame's region.
        i32 VertexBase = 0;
        u32 IndexBase = 0;
        vector<Gui::DrawRun> Runs;

        // The compiled UI + composite graph, held across frames and re-Compile()d only on Resize.
        // Its topology is invariant (record the draw list, then blend it over the scene), so the
        // per-frame variation — the run table, the extent, and the composite's sampled scene/UI
        // slots — is read live from these members inside the pass callbacks, never baked at compile.
        Unique<CompiledGraph> Graph;
        ResourceId UiId;
        ResourceId SceneId;
        ResourceId CompositeId;
        // The scene-output and UI-image bindless slots for the current frame, sampled by the
        // composite pass; refreshed each Render before the graph replays.
        TextureHandle SceneSlot;
        TextureHandle UiSlot;

        // The render-to-texture sink graph, built lazily on the first RenderToTarget. One pass —
        // clear the target transparent, replay the runs into it — whose imported target view is
        // bound live at Execute, so the same compiled graph records into any supplied target. Its
        // extent varies per target, read live from SinkExtent inside the callback.
        Unique<CompiledGraph> SinkGraph;
        ResourceId SinkTargetId;
        uvec2 SinkExtent{0};

        explicit Impl(const GuiScenePassInfo& info)
            : Context(info.Context), Extent(info.Extent), OutputFormat(info.OutputFormat)
        {
        }

        void CreateImages()
        {
            UiImage =
                Image::Create(Context, {
                                           .Name = "Gui UI Image",
                                           .Extent = {Extent.x, Extent.y, 1},
                                           .Format = OutputFormat,
                                           .Usage = ImageUsage::ColorAttachment |
                                                    ImageUsage::Sampled | ImageUsage::TransferSrc,
                                       });
            UiView = ImageView::Create(Context, {.Name = "Gui UI View", .Image = UiImage});

            CompositeImage =
                Image::Create(Context, {
                                           .Name = "Gui Composite Image",
                                           .Extent = {Extent.x, Extent.y, 1},
                                           .Format = OutputFormat,
                                           .Usage = ImageUsage::ColorAttachment |
                                                    ImageUsage::Sampled | ImageUsage::TransferSrc,
                                       });
            CompositeView =
                ImageView::Create(Context, {.Name = "Gui Composite View", .Image = CompositeImage});
        }

        // Replays the cached run table into the bound color target at the given extent: binds the
        // ring geometry, and per run its pipeline (rounded-rect SDF or MSDF text), set 0, push
        // block, and scissor. Shared by the overlay UI pass and the render-to-texture sink, so both
        // sinks record identical geometry into their respective targets.
        void RecordRuns(CommandBuffer& passCmd, uvec2 extent)
        {
            const BindlessRegistry& bindless = Context.GetBindlessRegistry();
            passCmd.SetViewport({0, 0}, extent);
            passCmd.BindVertexBuffer(VertexBuffer);
            passCmd.BindIndexBuffer(IndexBuffer, IndexType::U32);

            // The clip transform folds the UI scale in: positions are logical points over
            // extent / UiScale, so points-per-image-extent is UiScale / extent.
            const GuiPushConstants push{
                .InvScreenSize = vec2(UiScale / static_cast<f32>(extent.x),
                                      UiScale / static_cast<f32>(extent.y)),
                .GradientBuffer = GradientSlot.Index,
                .GradientBase = GradientBase,
                .Time = 0.0f,
            };

            optional<Gui::GuiPipeline> boundPipeline;

            for (const Gui::DrawRun& run : Runs)
            {
                if (run.IndexCount == 0)
                {
                    continue;
                }

                if (boundPipeline != run.Pipeline)
                {
                    passCmd.BindPipeline(run.Pipeline == Gui::GuiPipeline::Msdf ? MsdfPipeline
                                                                                : ShapePipeline);
                    // Set 0 is bound after the pipeline so the layout is established.
                    bindless.Bind(passCmd);
                    passCmd.PushConstants(push);
                    boundPipeline = run.Pipeline;
                }

                // The run's clip is already an absolute rectangle in logical points; the scissor
                // scales it onto the physical target (the vertex stage scales positions, but a
                // scissor is raw pixels). Unclipped runs scissor the whole surface.
                if (run.HasClip)
                {
                    const vec2 clipMin = run.Clip.Min * UiScale;
                    const vec2 clipSize = run.Clip.Size * UiScale;
                    const ivec2 offset{static_cast<i32>(clipMin.x), static_cast<i32>(clipMin.y)};
                    const uvec2 clipExtent{static_cast<u32>(std::ceil(clipSize.x)),
                                           static_cast<u32>(std::ceil(clipSize.y))};
                    passCmd.SetScissor(offset, clipExtent);
                }
                else
                {
                    passCmd.SetScissor({0, 0}, extent);
                }

                passCmd.DrawIndexed(run.IndexCount, 1, IndexBase + run.FirstIndex, VertexBase, 0);
            }
        }

        // Builds the one-pass render-to-texture sink graph and bakes its schedule once. The pass
        // clears its imported target transparent and replays the runs into it at SinkExtent; the
        // target view is supplied live at Execute, so the same compiled graph records into any
        // target of the pass's OutputFormat.
        void CompileSinkGraph()
        {
            RenderGraph graph(Context);
            SinkTargetId = graph.Import("Gui Sink Target");
            graph.AddPass("Gui Sink")
                .Color({
                    .Resource = SinkTargetId,
                    .Load = LoadOp::Clear,
                    .Store = StoreOp::Store,
                    .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
                })
                .Execute([this](PassContext& ctx) { RecordRuns(ctx.Cmd(), SinkExtent); });
            SinkGraph = graph.Compile();
        }

        // Builds the two-pass UI + composite graph and bakes its schedule once. The callbacks read
        // every per-frame input (Runs, Extent, the composite's SceneSlot/UiSlot) from this Impl at
        // record time, so the same compiled graph replays each frame against fresh geometry and
        // bindings; only a Resize (which changes the imported extent) re-Compile()s it.
        void CompileGraph()
        {
            RenderGraph graph(Context);
            UiId = graph.Import("Gui UI");
            SceneId = graph.Import("Gui Scene");
            CompositeId = graph.Import("Gui Composite");

            // Pass 1 — record the draw list into the UI image (cleared to transparent).
            graph.AddPass("Gui UI")
                .Color({
                    .Resource = UiId,
                    .Load = LoadOp::Clear,
                    .Store = StoreOp::Store,
                    .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
                })
                .Execute([this](PassContext& ctx) { RecordRuns(ctx.Cmd(), Extent); });

            // Pass 2 — composite: copy the scene output, then blend the UI over it.
            graph.AddPass("Gui Composite")
                .Color({
                    .Resource = CompositeId,
                    .Load = LoadOp::Clear,
                    .Store = StoreOp::Store,
                    .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
                })
                .Sample(SceneId)
                .Sample(UiId)
                .Execute(
                    [this](PassContext& ctx)
                    {
                        const BindlessRegistry& bindless = Context.GetBindlessRegistry();
                        CommandBuffer& passCmd = ctx.Cmd();
                        passCmd.SetViewport({0, 0}, Extent);
                        passCmd.SetScissor({0, 0}, Extent);

                        passCmd.BindPipeline(ScenePipeline);
                        bindless.Bind(passCmd);
                        passCmd.PushConstants(BlitPushConstants{
                            .Texture = SceneSlot.Index,
                            .Sampler = SamplerSlot.Index,
                        });
                        passCmd.DrawFullscreenTriangle();

                        passCmd.BindPipeline(OverlayPipeline);
                        bindless.Bind(passCmd);
                        passCmd.PushConstants(BlitPushConstants{
                            .Texture = UiSlot.Index,
                            .Sampler = SamplerSlot.Index,
                        });
                        passCmd.DrawFullscreenTriangle();
                    });

            Graph = graph.Compile();
        }
    };

    Unique<GuiScenePass> GuiScenePass::Create(const GuiScenePassInfo& info)
    {
        return Unique<GuiScenePass>(new GuiScenePass(info));
    }

    GuiScenePass::GuiScenePass(const GuiScenePassInfo& info) : m_Impl(CreateUnique<Impl>(info))
    {
        Context& context = info.Context;
        AssetManager& assets = info.Assets;

        auto load = [&](AssetId id) -> AssetHandle<Veng::Shader>
        {
            const AssetResult<AssetHandle<Veng::Shader>> result = assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(result.has_value(), "GuiScenePass: shader load failed: {}",
                      result.error().Detail);
            return *result;
        };

        m_Impl->FullscreenVs = load(FullscreenVertId);
        m_Impl->GatherFs = load(GatherFragId);
        m_Impl->GuiVs = load(GuiVertId);
        m_Impl->ShapeFs = load(GuiShapeFragId);
        m_Impl->MsdfFs = load(GuiMsdfFragId);

        // The gui vertex layout comes from the gui vertex shader's reflected layout id.
        optional<VertexBufferLayout> guiLayout;
        const ShaderInterface& guiVsInterface = m_Impl->GuiVs.Get()->Interface;
        if (guiVsInterface.VertexLayoutId.has_value())
        {
            const AssetResult<AssetHandle<Veng::VertexLayout>> layoutResult =
                assets.LoadSync<Veng::VertexLayout>(*guiVsInterface.VertexLayoutId);
            VE_ASSERT(layoutResult.has_value(), "GuiScenePass: gui vertex layout load failed: {}",
                      layoutResult.error().Detail);
            guiLayout = layoutResult->Get()->GetLayout();
        }

        m_Impl->GuiLayout = PipelineLayout::Create(
            context, {
                         .Name = "GuiScenePass Gui Layout",
                         .PushConstantRanges = {PushConstantRange::Of<GuiPushConstants>(
                             ShaderStage::Vertex | ShaderStage::Fragment)},
                     });

        auto buildGuiPipeline = [&](string_view name, const AssetHandle<Veng::Shader>& fs)
        {
            return GraphicsPipeline::Create(
                context,
                {
                    .Name = string(name),
                    .ColorAttachments = {{.Format = m_Impl->OutputFormat,
                                          .Blend = PremultipliedOver()}},
                    .VertexBufferLayout = guiLayout,
                    .PipelineLayout = m_Impl->GuiLayout,
                    .ShaderStages =
                        {
                            {.Stage = ShaderStage::Vertex, .Module = m_Impl->GuiVs.Get()->Module},
                            {.Stage = ShaderStage::Fragment, .Module = fs.Get()->Module},
                        },
                });
        };
        m_Impl->ShapePipeline = buildGuiPipeline("GuiScenePass Shape Pipeline", m_Impl->ShapeFs);
        m_Impl->MsdfPipeline = buildGuiPipeline("GuiScenePass Msdf Pipeline", m_Impl->MsdfFs);

        // The composite passes are fullscreen gather.frag blits: an opaque scene copy, then a
        // premultiplied-over UI blit on top.
        m_Impl->BlitLayout = PipelineLayout::Create(
            context, {
                         .Name = "GuiScenePass Blit Layout",
                         .PushConstantRanges = {PushConstantRange::Of<BlitPushConstants>(
                             ShaderStage::Fragment)},
                     });

        auto buildBlitPipeline = [&](string_view name, BlendState blend)
        {
            return GraphicsPipeline::Create(
                context, {
                             .Name = string(name),
                             .ColorAttachments = {{.Format = m_Impl->OutputFormat, .Blend = blend}},
                             .PipelineLayout = m_Impl->BlitLayout,
                             .ShaderStages =
                                 {
                                     {.Stage = ShaderStage::Vertex,
                                      .Module = m_Impl->FullscreenVs.Get()->Module},
                                     {.Stage = ShaderStage::Fragment,
                                      .Module = m_Impl->GatherFs.Get()->Module},
                                 },
                         });
        };
        m_Impl->ScenePipeline =
            buildBlitPipeline("GuiScenePass Scene Copy Pipeline", BlendState::Opaque());
        m_Impl->OverlayPipeline =
            buildBlitPipeline("GuiScenePass Overlay Pipeline", PremultipliedOver());

        m_Impl->Sampler = Sampler::Create(context, {
                                                       .Name = "GuiScenePass Sampler",
                                                       .MagFilter = Filter::Linear,
                                                       .MinFilter = Filter::Linear,
                                                       .AddressModeU = AddressMode::ClampToEdge,
                                                       .AddressModeV = AddressMode::ClampToEdge,
                                                       .AddressModeW = AddressMode::ClampToEdge,
                                                   });
        m_Impl->SamplerSlot = context.GetBindlessRegistry().Register(m_Impl->Sampler);

        // Ring the geometry per frame-in-flight, sized to a generous UI budget.
        constexpr u32 MaxVertices = 1u << 16;
        constexpr u32 MaxIndices = 1u << 17;
        constexpr u32 MaxGradients = 1u << 12;
        m_Impl->FramesInFlight = context.GetMaxFramesInFlight();
        m_Impl->VertexRegionBytes = static_cast<u64>(MaxVertices) * sizeof(Gui::GuiVertex);
        m_Impl->IndexRegionBytes = static_cast<u64>(MaxIndices) * sizeof(u32);
        m_Impl->GradientRegionBytes = static_cast<u64>(MaxGradients) * sizeof(Gui::GpuGradient);

        m_Impl->VertexBuffer =
            Buffer::Create(context, {
                                        .Name = "GuiScenePass Vertices",
                                        .Size = m_Impl->VertexRegionBytes * m_Impl->FramesInFlight,
                                        .Usage = BufferUsage::Vertex,
                                        .HostMapped = true,
                                    });
        m_Impl->IndexBuffer =
            Buffer::Create(context, {
                                        .Name = "GuiScenePass Indices",
                                        .Size = m_Impl->IndexRegionBytes * m_Impl->FramesInFlight,
                                        .Usage = BufferUsage::Index,
                                        .HostMapped = true,
                                    });
        m_Impl->GradientBuffer = Buffer::Create(
            context, {
                         .Name = "GuiScenePass Gradients",
                         .Size = m_Impl->GradientRegionBytes * m_Impl->FramesInFlight,
                         .Usage = BufferUsage::Storage,
                         .HostMapped = true,
                     });
        m_Impl->GradientSlot = context.GetBindlessRegistry().Register(m_Impl->GradientBuffer);

        m_Impl->CreateImages();
        m_Impl->CompileGraph();
    }

    GuiScenePass::~GuiScenePass()
    {
        m_Impl->Context.GetBindlessRegistry().Release(m_Impl->SamplerSlot);
        m_Impl->Context.GetBindlessRegistry().Release(m_Impl->GradientSlot);
    }

    void GuiScenePass::SetDrawList(const Gui::DrawList& drawList)
    {
        const u32 frame = m_Impl->Context.GetCurrentFrameInFlight();

        const auto& vertices = drawList.GetVertices();
        const auto& indices = drawList.GetIndices();
        const auto& gradients = drawList.GetGradients();

        const u64 vertexBytes = vertices.size() * sizeof(Gui::GuiVertex);
        const u64 indexBytes = indices.size() * sizeof(u32);
        const u64 gradientBytes = gradients.size() * sizeof(Gui::GpuGradient);
        VE_ASSERT(vertexBytes <= m_Impl->VertexRegionBytes,
                  "GuiScenePass draw list exceeds the vertex ring capacity ({} > {})", vertexBytes,
                  m_Impl->VertexRegionBytes);
        VE_ASSERT(indexBytes <= m_Impl->IndexRegionBytes,
                  "GuiScenePass draw list exceeds the index ring capacity ({} > {})", indexBytes,
                  m_Impl->IndexRegionBytes);
        VE_ASSERT(gradientBytes <= m_Impl->GradientRegionBytes,
                  "GuiScenePass draw list exceeds the gradient ring capacity ({} > {})",
                  gradientBytes, m_Impl->GradientRegionBytes);

        const u64 vertexByteBase = static_cast<u64>(frame) * m_Impl->VertexRegionBytes;
        m_Impl->VertexBase = static_cast<i32>(vertexByteBase / sizeof(Gui::GuiVertex));
        const u64 indexByteBase = static_cast<u64>(frame) * m_Impl->IndexRegionBytes;
        m_Impl->IndexBase = static_cast<u32>(indexByteBase / sizeof(u32));
        const u64 gradientByteBase = static_cast<u64>(frame) * m_Impl->GradientRegionBytes;
        m_Impl->GradientBase = static_cast<u32>(gradientByteBase / sizeof(Gui::GpuGradient));

        if (vertexBytes > 0)
        {
            auto* vertexDst = static_cast<u8*>(m_Impl->VertexBuffer->GetMappedData());
            std::memcpy(vertexDst + vertexByteBase, vertices.data(), vertexBytes);
        }
        if (indexBytes > 0)
        {
            auto* indexDst = static_cast<u8*>(m_Impl->IndexBuffer->GetMappedData());
            std::memcpy(indexDst + indexByteBase, indices.data(), indexBytes);
        }
        if (gradientBytes > 0)
        {
            auto* gradientDst = static_cast<u8*>(m_Impl->GradientBuffer->GetMappedData());
            std::memcpy(gradientDst + gradientByteBase, gradients.data(), gradientBytes);
        }

        m_Impl->Runs = drawList.GetRuns();
    }

    void GuiScenePass::SetUiScale(const f32 scale)
    {
        VE_ASSERT(scale > 0.0f, "GuiScenePass::SetUiScale: scale {} must be positive", scale);
        m_Impl->UiScale = scale;
    }

    void GuiScenePass::Resize(uvec2 extent)
    {
        if (extent == m_Impl->Extent || extent.x == 0 || extent.y == 0)
        {
            return;
        }
        m_Impl->Extent = extent;
        m_Impl->CreateImages();
        m_Impl->CompileGraph();
    }

    void GuiScenePass::Render(CommandBuffer& cmd, const Ref<ImageView>& sceneOutput)
    {
        Impl& impl = *m_Impl;
        BindlessRegistry& bindless = impl.Context.GetBindlessRegistry();

        // The scene output and the UI image are both sampled by the composite pass, out of the
        // graph, so bind their bindless slots for this frame and release them after; the composite
        // callback reads them back through impl.SceneSlot/UiSlot at record time.
        impl.SceneSlot = bindless.Register(sceneOutput);
        impl.UiSlot = bindless.Register(impl.UiView);

        // The scene output arrives already in a sampleable layout (the viewport transitioned it);
        // ensure it regardless, since a test may hand a freshly written target.
        cmd.PrepareForAccess(sceneOutput, AccessKind::Sample);

        // Replay the graph baked at construction/Resize against this frame's geometry and imports —
        // the topology is invariant, so no per-frame recompile.
        const RenderGraph::ImportBinding bindings[] = {
            {.Id = impl.UiId, .View = impl.UiView},
            {.Id = impl.SceneId, .View = sceneOutput},
            {.Id = impl.CompositeId, .View = impl.CompositeView},
        };
        impl.Graph->Execute(cmd, bindings);

        bindless.Release(impl.SceneSlot);
        bindless.Release(impl.UiSlot);
    }

    void GuiScenePass::RenderToTarget(CommandBuffer& cmd, Gui::RenderTarget& target)
    {
        Impl& impl = *m_Impl;
        VE_ASSERT(target.GetFormat() == impl.OutputFormat,
                  "GuiScenePass::RenderToTarget: target format must match the pass OutputFormat");

        impl.SinkExtent = target.GetExtent();
        if (!impl.SinkGraph)
        {
            impl.CompileSinkGraph();
        }

        const RenderGraph::ImportBinding binding{.Id = impl.SinkTargetId,
                                                 .View = target.GetOutput()};
        impl.SinkGraph->Execute(cmd, {&binding, 1});

        // Leave the target in a sampleable layout for a later sampler in the frame — the
        // producer-before-consumer handoff a downstream material reads across.
        cmd.PrepareForAccess(target.GetOutput(), AccessKind::Sample);
    }

    const Ref<ImageView>& GuiScenePass::GetOutput() const
    {
        return m_Impl->CompositeView;
    }

    const Ref<ImageView>& GuiScenePass::GetUiImage() const
    {
        return m_Impl->UiView;
    }
}
