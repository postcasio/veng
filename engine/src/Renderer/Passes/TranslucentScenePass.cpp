#include "TranslucentScenePass.h"

#include <algorithm>

#include <fmt/format.h>

#include <Veng/Assert.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/GBuffer.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/ShaderInterface.h>

#include "../HalfResTranslucency.h"

namespace Veng::Renderer
{
    void TranslucentScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        RenderGraph::PassBuilder builder =
            graph.AddPass(m_HalfResolution ? "Scene Translucent Half" : "Scene Translucent");
        if (m_HalfResolution)
        {
            // The layer target accumulates over nothing: cleared to transparent, the straight
            // alpha blend leaves it holding (premultiplied color, coverage), which is exactly
            // what the composite's (One, OneMinusSrcAlpha) blend consumes.
            builder.Color({
                .Resource = m_TargetId,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
            });
        }
        else
        {
            builder.Color({
                // Alpha-blend into the lit scene color (blend enabled on the pipeline).
                .Resource = m_TargetId,
                .Load = LoadOp::Load,
                .Store = StoreOp::Store,
            });
        }
        if (m_MaskId.IsValid())
        {
            // This pass is the only writer of the scene-side mask, so it clears the target at begin
            // rather than costing the frame a separate clear: a pixel no declaring material covers
            // reads 0 and leaves the bloom bright-pass to decide the glow there on its own. The
            // clear covers the whole render allocation, so the area outside this frame's rendered
            // sub-rect carries no amplitude into the promotion.
            builder.Color({
                .Resource = m_MaskId,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
            });
        }
        builder.Depth({
            // The opaque depth bound read-only (the pipeline disables depth writes):
            // translucents depth-test against the resolved opaque scene so they are
            // occluded correctly, without occluding one another.
            .Resource = m_DepthId,
            .Load = LoadOp::Load,
            .Store = StoreOp::Store,
        });
        if (m_SceneColorId.IsValid())
        {
            builder.Sample(m_SceneColorId);
        }
        if (m_SceneDepthId.IsValid())
        {
            builder.Sample(m_SceneDepthId);
        }
        // A forward-lit material samples the shadow atlases, as the deferred lighting pass does.
        if (io.ShadowMap.IsValid())
        {
            builder.Sample(io.ShadowMap);
        }
        if (io.PunctualShadowMap.IsValid())
        {
            builder.Sample(io.PunctualShadowMap);
        }
        builder.Execute([this](PassContext& inner) { Record(Wrap(inner)); });
    }

    bool TranslucentScenePass::IsForwardLit(const Material& material)
    {
        return std::ranges::any_of(material.GetFragmentInterface().Bindings,
                                   [](const ShaderBinding& binding)
                                   {
                                       return binding.Set == ForwardLightingIblSet ||
                                              binding.Set == ForwardLightingShadowSet;
                                   });
    }

    Ref<PipelineLayout> TranslucentScenePass::ForwardLitLayoutFor(const Material& parent) const
    {
        const Ref<PipelineLayout>& reflected = parent.GetPipelineLayout();
        const vector<Ref<DescriptorSetLayout>>& authored = reflected->GetDescriptorSetLayouts();
        // Author set 0 is set 3, the DrawData every surface-drawn fragment declares; the two after
        // it are the forward-lighting sets, and nothing may follow them.
        VE_ASSERT(
            !authored.empty() &&
                authored.size() <= ForwardLightingShadowSet - BindlessRegistry::FirstUserSet + 1,
            "TranslucentScenePass: forward-lit material '{}' declares {} author sets; sets 3-{} "
            "are the DrawData and forward-lighting sets and a Translucent fragment may "
            "declare no others",
            parent.GetName(), authored.size(), ForwardLightingShadowSet);
        return PipelineLayout::Create(
            m_Context,
            {
                .Name = fmt::format("Translucent Forward-Lit Layout ({})", parent.GetName()),
                .DescriptorSetLayouts = {authored.front(), m_Forward.IblLayout,
                                         m_Forward.ShadowLayout},
                .PushConstantRanges = reflected->GetPushConstantRanges(),
            });
    }

    const TranslucentScenePass::CachedPipeline&
    TranslucentScenePass::PipelineFor(const MaterialInstance& material) const
    {
        const Material* parent = material.GetParent().Get();
        const auto it = m_Pipelines.find(parent);
        if (it != m_Pipelines.end())
        {
            return it->second;
        }

        // A forward-lit fragment reads the renderer's IBL and shadow sets, so its pipeline is built
        // against their layouts: the reflected shadow set cannot carry the immutable comparison
        // sampler, and only an identically-defined layout accepts the renderer's set.
        const bool forwardLit = IsForwardLit(*parent);
        const Ref<PipelineLayout> layout =
            forwardLit ? ForwardLitLayoutFor(*parent) : material.GetPipelineLayout();

        vector<PipelineAttachmentInfo> attachments = {
            {.Format = m_TargetFormat,
             .Blend = parent->GetTranslucentBlend() == TranslucentBlend::Additive
                          ? BlendState::AlphaAdditive()
                          : BlendState::AlphaBlend()}};
        if (m_MaskId.IsValid())
        {
            // The mask accumulates additively, so two declaring surfaces over one pixel glow by
            // the sum of the amplitudes they ask for rather than by the nearer one's alone. The
            // float target does not bound that sum: the mask is an amplitude, and clamping it
            // would cap a glow at the radiance it was decoupled from. Writes are off unless this
            // material's fragment declares the output — the value an undeclared stage would leave
            // in the slot is undefined.
            attachments.push_back({
                .Format = m_MaskFormat,
                .Blend = BlendState::Additive(),
                .Write = parent->IsBloomMaskWriter(),
            });
        }

        Ref<GraphicsPipeline> pipeline = GraphicsPipeline::Create(
            m_Context,
            {
                .Name = fmt::format("Translucent Pipeline ({})", material.GetName()),
                .ColorAttachments = std::move(attachments),
                .DepthAttachmentFormat = GBuffer::DepthFormat,
                .VertexBufferLayout = Mesh::CanonicalLayout(),
                // The surface vertex stage reads the per-draw candidate id as an
                // instance-rate attribute on binding 1 (fetched at firstInstance).
                .InstanceCandidateId = true,
                .PipelineLayout = layout,
                .ShaderStages =
                    {
                        {.Stage = ShaderStage::Vertex, .Module = material.GetVertexModule()},
                        {.Stage = ShaderStage::Fragment, .Module = material.GetFragmentModule()},
                    },
                // The material's authored cull mode (Back when unauthored, the opaque
                // surface convention) — a two-sided translucent surface authors None.
                .CullMode = parent->GetCullMode(),
                .DepthTestEnable = true,
                .DepthWriteEnable = false,
                // Reverse-Z: a nearer fragment has larger depth.
                .DepthCompareOp = CompareOp::GreaterOrEqual,
            });

        return m_Pipelines
            .emplace(parent,
                     CachedPipeline{.Pipeline = std::move(pipeline), .ForwardLit = forwardLit})
            .first->second;
    }

    void TranslucentScenePass::Record(const ScenePassContext& ctx) const
    {
        CommandBuffer& cmd = ctx.Cmd();
        const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
        const TranslucentDrawPlan& plan = *m_Plan;

        const uvec2 fullExtent = ctx.View().RenderExtent;
        const uvec2 renderExtent = m_HalfResolution ? HalfResExtent(fullExtent) : fullExtent;
        cmd.SetViewport({0, 0}, renderExtent);
        cmd.SetScissor({0, 0}, renderExtent);

        if (plan.Draws.empty())
        {
            return;
        }

        // The instance-rate candidate-id buffer (binding 1) is bound once; each draw's
        // firstInstance selects the candidate id that indexes DrawData.
        cmd.GetNative().CommandBuffer.bindVertexBuffers(1, GetVkBuffer(*plan.CandidateIdBuffer),
                                                        {0});

        // Back-to-front: bind each draw's material pipeline (rebound only on a change, since
        // sorting interleaves materials by depth), the bindless registry, and the shared
        // DrawData set, then the mesh buffers, then draw the submesh. The surface push is
        // rebound with the pipeline (its layout is per-material). Translucent materials push
        // no selector (they read it from DrawData), so Material::Bind only binds the
        // pipeline; the selector rides each draw's DrawData record via the candidate id.
        // The shadow rings are renderer-owned and framesInFlight-deep, so their dynamic offsets
        // are the frame-in-flight index, exactly as the deferred lighting pass binds them.
        const u32 frameSlot = m_Context.GetCurrentFrameInFlight();
        const GraphicsPipeline* lastPipeline = nullptr;
        const Mesh* lastMesh = nullptr;
        for (const TranslucentDraw& draw : plan.Draws)
        {
            const CachedPipeline& cached = PipelineFor(*draw.Material);
            const Ref<GraphicsPipeline>& pipeline = cached.Pipeline;
            if (pipeline.get() != lastPipeline)
            {
                cmd.BindPipeline(pipeline);
                registry.Bind(cmd);
                if (cached.ForwardLit)
                {
                    cmd.BindDescriptorSets(
                        {plan.DrawDataSet.get(), m_Forward.IblSet.get(), m_Forward.ShadowSet.get()},
                        BindlessRegistry::FirstUserSet, PipelineBindPoint::Graphics,
                        {frameSlot * m_Forward.ShadowRingStride,
                         frameSlot * m_Forward.PunctualRingStride});
                }
                else
                {
                    cmd.BindDescriptorSets({plan.DrawDataSet.get()},
                                           BindlessRegistry::FirstUserSet);
                }
                cmd.PushConstants(plan.Push);
                lastPipeline = pipeline.get();
                lastMesh = nullptr;
            }
            if (draw.SourceMesh != lastMesh)
            {
                cmd.BindVertexBuffer(draw.SourceMesh->GetVertexBuffer());
                cmd.BindIndexBuffer(draw.SourceMesh->GetIndexBuffer());
                lastMesh = draw.SourceMesh;
            }
            cmd.DrawIndexed(draw.IndexCount, 1, draw.FirstIndex, 0, draw.CandidateId);
        }
    }
}
