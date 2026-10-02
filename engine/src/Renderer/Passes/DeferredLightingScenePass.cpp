#include "DeferredLightingScenePass.h"

#include "../LightTileCuller.h"

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/GraphicsPipeline.h>

namespace Veng::Renderer
{
    void DeferredLightingScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        const TextureHandle albedoHandle = io.AlbedoHandle;
        const TextureHandle normalHandle = io.NormalHandle;
        const TextureHandle ormHandle = io.OrmHandle;
        const TextureHandle depthHandle = io.DepthHandle;
        const TextureHandle emissiveHandle = io.EmissiveHandle;
        const TextureHandle ssaoHandle = io.SsaoHandle;
        const SamplerHandle samplerHandle = io.SamplerHandle;
        const bool useSsao = m_UseSsao;
        const Ref<DescriptorSet> shadowSet = m_ShadowSet;
        const u32 shadowRingStride = m_ShadowRingStride;
        const u32 punctualRingStride = m_PunctualRingStride;

        // The tile cull reduces the depth this pass reads and writes the masks it reads, so it is
        // declared first; its mask write orders ahead of the read below.
        if (m_LightTiles != nullptr)
        {
            m_LightTiles->DeclareCull(graph, io.GBufferDepth, depthHandle);
        }

        RenderGraph::PassBuilder builder = graph.AddPass("Deferred Lighting");
        builder
            .Color({
                .Resource = m_WriteToOutput ? io.Output : io.Hdr,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            })
            .Sample(io.GBufferAlbedo)
            .Sample(io.GBufferNormal)
            .Sample(io.GBufferOrm)
            .Sample(io.GBufferDepth)
            .Sample(io.GBufferEmissive);

        // Declaring the shadow/punctual maps sampled drives the graph-derived
        // depth-attachment → shader-read barriers. The atlases reach the
        // lighting shader through set 3 (off bindless); the declarations here
        // are only for barrier derivation.
        if (io.ShadowMap.IsValid())
        {
            builder.Sample(io.ShadowMap);
        }
        if (io.PunctualShadowMap.IsValid())
        {
            builder.Sample(io.PunctualShadowMap);
        }

        if (useSsao)
        {
            builder.Sample(io.Ssao);
        }

        // The masks reach the shader through the set-0 storage-buffer array the view block names;
        // the declaration is for barrier derivation.
        if (m_LightTiles != nullptr)
        {
            builder.StorageBufferRead(m_LightTiles->GetMaskId());
        }

        const Ref<DescriptorSet> iblSet = m_IblSet;
        builder.Execute(
            [this, albedoHandle, normalHandle, ormHandle, depthHandle, emissiveHandle, ssaoHandle,
             samplerHandle, useSsao, shadowSet, shadowRingStride, punctualRingStride,
             iblSet](PassContext& inner)
            {
                const ScenePassContext ctx = Wrap(inner);
                CommandBuffer& cmd = ctx.Cmd();
                const BindlessRegistry& registry = m_Context.GetBindlessRegistry();

                cmd.BindPipeline(m_Pipeline);
                const uvec2 renderExtent = ctx.View().RenderExtent;
                cmd.SetViewport({0, 0}, renderExtent);
                cmd.SetScissor({0, 0}, renderExtent);
                registry.Bind(cmd);

                // Bind set 3 (the shadow system: both atlases, comparison sampler, and
                // both ring-buffered dynamic uniforms) and set 4 (the IBL maps + sampler,
                // always valid) — the two author sets above the typed bindless registries.
                // The shadow rings are renderer-owned and framesInFlight-deep, so their
                // dynamic offset is the frame-in-flight index — not the shared view-constants
                // slot, which rings per viewport render. The IBL set has no dynamic descriptors
                // so the offsets still map to set 3's two in binding order.
                const u32 frameSlot = m_Context.GetCurrentFrameInFlight();
                cmd.BindDescriptorSets(DescriptorSetBindInfo{
                    .Sets = {shadowSet, iblSet},
                    .FirstSet = 3,
                    .PipelineBindPoint = PipelineBindPoint::Graphics,
                    .DynamicOffsets = {frameSlot * shadowRingStride,
                                       frameSlot * punctualRingStride},
                });

                // The light state (bases, count, ambient arm and parameters, LTC LUTs) rides the
                // view block this index selects.
                if (useSsao)
                {
                    cmd.PushConstants(SsaoLightingPushConstants{
                        .AlbedoTexture = albedoHandle.Index,
                        .NormalTexture = normalHandle.Index,
                        .OrmTexture = ormHandle.Index,
                        .DepthTexture = depthHandle.Index,
                        .EmissiveTexture = emissiveHandle.Index,
                        .Sampler = samplerHandle.Index,
                        .ViewConstantsIndex = registry.GetCurrentViewConstantsIndex(),
                        .SsaoTexture = ssaoHandle.Index,
                    });
                }
                else
                {
                    cmd.PushConstants(LightingPushConstants{
                        .AlbedoTexture = albedoHandle.Index,
                        .NormalTexture = normalHandle.Index,
                        .OrmTexture = ormHandle.Index,
                        .DepthTexture = depthHandle.Index,
                        .EmissiveTexture = emissiveHandle.Index,
                        .Sampler = samplerHandle.Index,
                        .ViewConstantsIndex = registry.GetCurrentViewConstantsIndex(),
                    });
                }
                cmd.DrawFullscreenTriangle();
            });
    }
}
