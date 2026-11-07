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
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/ShaderInterface.h>

#include "../HalfResTranslucency.h"
#include "../SphereTerrainLayout.h"
#include "../SphereTerrainSystem.h"

namespace Veng::Renderer
{
    static_assert(TranslucentSphereTerrainSet == ForwardLightingShadowSet + 1,
                  "a translucent level layer's terrain set follows the forward-lighting sets");

    TranslucentScenePass::TranslucentScenePass(
        Context& context, const uvec2 extent, const TranslucentDrawPlan* plan,
        const ResourceId targetId, const ResourceId depthId, const ResourceId sceneColorId,
        const ResourceId sceneDepthId, const Format targetFormat, const ResourceId maskId,
        const Format maskFormat, const ResourceId reactiveId, const Format reactiveFormat,
        const ResourceId surfaceFlagsId, const TranslucentPassKind kind, const u32 segment,
        ForwardLightingSets forward, Ref<PipelineCache> pipelines)
        : m_Context(context), m_Extent(extent), m_Plan(plan), m_TargetId(targetId),
          m_DepthId(depthId), m_SceneColorId(sceneColorId), m_SceneDepthId(sceneDepthId),
          m_TargetFormat(targetFormat), m_MaskId(maskId), m_MaskFormat(maskFormat),
          m_ReactiveId(reactiveId), m_ReactiveFormat(reactiveFormat),
          m_SurfaceFlagsId(surfaceFlagsId), m_Kind(kind), m_Segment(segment),
          m_HalfResolution(kind == TranslucentPassKind::HalfLayer),
          m_PostResolve(kind == TranslucentPassKind::PostResolve), m_Forward(std::move(forward)),
          m_Pipelines(pipelines ? std::move(pipelines) : std::make_shared<PipelineCache>())
    {
        VE_ASSERT(!m_SurfaceFlagsId.IsValid() || kind == TranslucentPassKind::First ||
                      kind == TranslucentPassKind::Continuation,
                  "TranslucentScenePass: only a full-resolution scene pass writes surface flags");
    }

    Ref<TranslucentScenePass::PipelineCache> TranslucentScenePass::CreatePipelineCache()
    {
        return std::make_shared<PipelineCache>();
    }

    void TranslucentScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        // Segment 0 keeps the names a single-segment frame always had.
        const string name = [this]
        {
            if (m_PostResolve)
            {
                return string{"Post-Resolve Translucent"};
            }
            const char* base = m_HalfResolution ? "Scene Translucent Half" : "Scene Translucent";
            const u32 index =
                m_Kind == TranslucentPassKind::Continuation ? m_Segment + 1 : m_Segment;
            return index == 0 ? string{base} : fmt::format("{} {}", base, index);
        }();
        RenderGraph::PassBuilder builder = graph.AddPass(name);
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
        if (m_MaskId.IsValid() && m_Kind == TranslucentPassKind::First)
        {
            // The first translucent pass is the first writer of the scene-side mask, so it clears
            // the target at begin rather than costing the frame a separate clear: a pixel no
            // declaring material covers reads 0 and leaves the bloom bright-pass to decide the glow
            // there on its own. The clear covers the whole render allocation, so the area outside
            // this frame's rendered sub-rect carries no amplitude into the promotion.
            builder.Color({
                .Resource = m_MaskId,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
            });
        }
        else if (m_MaskId.IsValid())
        {
            builder.Color({.Resource = m_MaskId, .Load = LoadOp::Load, .Store = StoreOp::Store});
        }
        // A reactive material writes location 2 and a metering exclusion location 3 whatever the
        // frame wires before them.
        const bool flags = m_SurfaceFlagsId.IsValid();
        if (!m_MaskId.IsValid() && (m_ReactiveId.IsValid() || flags))
        {
            builder.UnboundColor();
        }
        if (m_ReactiveId.IsValid() && m_Kind == TranslucentPassKind::First)
        {
            // The first translucent pass records every frame, so it owns the reactive mask's clear
            // as it owns the bloom mask's: every later writer — a further translucent pass, the
            // ribbons, sprites and particles — loads it, in a frame drawing no translucent too.
            builder.Color({
                .Resource = m_ReactiveId,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
            });
        }
        else if (m_ReactiveId.IsValid())
        {
            // A declaring material adds to the mask the first pass cleared.
            builder.Color(
                {.Resource = m_ReactiveId, .Load = LoadOp::Load, .Store = StoreOp::Store});
        }
        else if (flags)
        {
            builder.UnboundColor();
        }
        if (flags)
        {
            // The g-buffer's surface flags, raised in place for the auto-exposure meter after it.
            builder.Color(
                {.Resource = m_SurfaceFlagsId, .Load = LoadOp::Load, .Store = StoreOp::Store});
        }
        // The post-resolve allocation has no depth buffer and the grab predates it, so that layer
        // binds neither and draws over the scene.
        if (!m_PostResolve)
        {
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
        // ...and visits only the lights its tile's forward mask names.
        if (io.LightTileMasks.IsValid())
        {
            builder.StorageBufferRead(io.LightTileMasks);
        }
        if (m_HalfResolution)
        {
            // The layer is read only by its composite, which skips an idle frame with it, so the
            // cleared target goes unread.
            builder.SkipWhenUnread([this] { return m_Plan->Draws.empty(); });
        }
        else if (m_Kind == TranslucentPassKind::Continuation ||
                 m_Kind == TranslucentPassKind::PostResolve)
        {
            // Every attachment loads and stores, so a frame whose segment drew nothing here skips.
            builder.SkipWhen([this] { return m_Plan->Draws.empty(); });
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

    Ref<PipelineLayout> TranslucentScenePass::TerrainLayoutFor(const Material& parent) const
    {
        const Ref<PipelineLayout>& reflected = parent.GetPipelineLayout();
        const vector<Ref<DescriptorSetLayout>>& authored = reflected->GetDescriptorSetLayouts();
        // The material loader lays a translucent terrain material out as its DrawData set, two
        // empty sets for forward lighting and the terrain's own set.
        constexpr usize terrainIndex = TranslucentSphereTerrainSet - BindlessRegistry::FirstUserSet;
        VE_ASSERT(authored.size() == terrainIndex + 1,
                  "TranslucentScenePass: sphere-terrain material '{}' carries {} author sets, not "
                  "the DrawData, forward-lighting and terrain sets 3-{}",
                  parent.GetName(), authored.size(), TranslucentSphereTerrainSet);
        return PipelineLayout::Create(
            m_Context, {
                           .Name = fmt::format("Translucent Terrain Layout ({})", parent.GetName()),
                           .DescriptorSetLayouts = {authored.front(), m_Forward.IblLayout,
                                                    m_Forward.ShadowLayout, authored[terrainIndex]},
                           .PushConstantRanges = reflected->GetPushConstantRanges(),
                       });
    }

    const TranslucentScenePass::CachedPipeline&
    TranslucentScenePass::PipelineFor(const MaterialInstance& material) const
    {
        const Material* parent = material.GetParent().Get();
        const auto it = m_Pipelines->Pipelines.find(parent);
        if (it != m_Pipelines->Pipelines.end())
        {
            return it->second;
        }

        // A forward-lit fragment reads the renderer's IBL and shadow sets, so its pipeline is built
        // against their layouts: the reflected shadow set cannot carry the immutable comparison
        // sampler, and only an identically-defined layout accepts the renderer's set.
        // A level layer draws the terrain's grid through the terrain's stage, which reads no vertex
        // input and finds its tile through the instance index; it binds the forward-lighting sets
        // whatever its fragment declares, and the terrain set after them.
        const bool terrain = parent->IsSphereTerrain();
        const bool forwardLit = terrain || IsForwardLit(*parent);
        const Ref<PipelineLayout> layout = terrain      ? TerrainLayoutFor(*parent)
                                           : forwardLit ? ForwardLitLayoutFor(*parent)
                                                        : material.GetPipelineLayout();

        vector<PipelineAttachmentInfo> attachments = {
            {.Format = m_TargetFormat,
             .Blend = parent->GetTranslucentBlend() == TranslucentBlend::Additive
                          ? BlendState::AlphaAdditive()
                          : BlendState::AlphaBlend()}};
        const bool flags = m_SurfaceFlagsId.IsValid();
        if (m_MaskId.IsValid() || m_ReactiveId.IsValid() || flags)
        {
            // The mask accumulates additively, so two declaring surfaces over one pixel glow by
            // the sum of the amplitudes they ask for rather than by the nearer one's alone. The
            // float target does not bound that sum: the mask is an amplitude, and clamping it
            // would cap a glow at the radiance it was decoupled from. Writes are off unless this
            // material's fragment declares the output — the value an undeclared stage would leave
            // in the slot is undefined. With no mask wired the slot is unbound.
            attachments.push_back({
                .Format = m_MaskId.IsValid() ? m_MaskFormat : Format::Undefined,
                .Blend = BlendState::Additive(),
                .Write = m_MaskId.IsValid() && parent->IsBloomMaskWriter(),
            });
        }
        if (m_ReactiveId.IsValid() || flags)
        {
            // Reactive amounts sum like the mask's, saturating in the R8 target.
            attachments.push_back({
                .Format = m_ReactiveId.IsValid() ? m_ReactiveFormat : Format::Undefined,
                .Blend = BlendState::Additive(),
                .Write = m_ReactiveId.IsValid() && parent->IsReactiveWriter(),
            });
        }
        if (flags)
        {
            // The fragment writes zero colour and a flags code in alpha, so a MAX blend leaves the
            // stored occlusion, roughness and metallic as they are and raises the code to one
            // carrying the meter's bit.
            attachments.push_back({
                .Format = GBuffer::ORMFormat,
                .Blend =
                    BlendState{
                        .Enable = true,
                        .SrcColorFactor = BlendFactor::One,
                        .DstColorFactor = BlendFactor::One,
                        .ColorOp = BlendOp::Max,
                        .SrcAlphaFactor = BlendFactor::One,
                        .DstAlphaFactor = BlendFactor::One,
                        .AlphaOp = BlendOp::Max,
                    },
                .Write = parent->IsMeteringExcluder(),
            });
        }

        Ref<GraphicsPipeline> pipeline = GraphicsPipeline::Create(
            m_Context,
            {
                .Name = fmt::format("Translucent Pipeline ({})", material.GetName()),
                .ColorAttachments = std::move(attachments),
                .DepthAttachmentFormat = m_PostResolve ? Format::Undefined : GBuffer::DepthFormat,
                .VertexBufferLayout =
                    terrain ? optional<VertexBufferLayout>{} : Mesh::CanonicalLayout(),
                // The surface vertex stage reads the per-draw candidate id as an
                // instance-rate attribute on binding 1 (fetched at firstInstance).
                .InstanceCandidateId = !terrain,
                .PipelineLayout = layout,
                .ShaderStages =
                    {
                        {.Stage = ShaderStage::Vertex, .Module = material.GetVertexModule()},
                        {.Stage = ShaderStage::Fragment, .Module = material.GetFragmentModule()},
                    },
                // The material's authored cull mode (Back when unauthored, the opaque
                // surface convention) — a two-sided translucent surface authors None.
                .CullMode = parent->GetCullMode(),
                .DepthTestEnable = !m_PostResolve,
                .DepthWriteEnable = false,
                // Reverse-Z: a nearer fragment has larger depth.
                .DepthCompareOp = CompareOp::GreaterOrEqual,
            });

        return m_Pipelines->Pipelines
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
        const uvec2 renderExtent = m_PostResolve      ? ctx.View().PostResolveExtent
                                   : m_HalfResolution ? HalfResExtent(fullExtent)
                                                      : fullExtent;
        cmd.SetViewport({0, 0}, renderExtent);
        cmd.SetScissor({0, 0}, renderExtent);

        if (plan.Draws.empty())
        {
            return;
        }

        // The instance-rate candidate-id buffer (binding 1) is bound once; each draw's
        // firstInstance selects the candidate id that indexes DrawData.
        cmd.BindInstanceBuffer(plan.CandidateIdBuffer);

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
            if (draw.Terrain != nullptr)
            {
                // The terrain binds its grid's indices and pushes its own surface.
                draw.Terrain->RecordTranslucent(cmd, draw, plan.Push);
                lastPipeline = nullptr;
                lastMesh = nullptr;
                continue;
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
