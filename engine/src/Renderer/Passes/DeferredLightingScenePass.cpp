#include "DeferredLightingScenePass.h"

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
        const TextureHandle ltcMatHandle = io.LtcMatHandle;
        const TextureHandle ltcMagHandle = io.LtcMagHandle;
        const SamplerHandle samplerHandle = io.SamplerHandle;
        const bool useSsao = m_UseSsao;
        const bool skylight = m_Skylight;
        const bool iblAllowed = m_IblAllowed;
        const Ref<DescriptorSet> shadowSet = m_ShadowSet;
        const u32 shadowRingStride = m_ShadowRingStride;
        const u32 punctualRingStride = m_PunctualRingStride;

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
        // lighting shader through set 1 (off bindless); the declarations here
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

        const Ref<DescriptorSet> iblSet = m_IblSet;
        const u32 prefilterMipCount = m_PrefilterMipCount;
        builder.Execute(
            [this, albedoHandle, normalHandle, ormHandle, depthHandle, emissiveHandle, ssaoHandle,
             ltcMatHandle, ltcMagHandle, samplerHandle, useSsao, skylight, iblAllowed, shadowSet,
             shadowRingStride, punctualRingStride, iblSet, prefilterMipCount](PassContext& inner)
            {
                const ScenePassContext ctx = Wrap(inner);
                CommandBuffer& cmd = ctx.Cmd();
                const BindlessRegistry& registry = m_Context.GetBindlessRegistry();

                cmd.BindPipeline(m_Pipeline);
                const uvec2 renderExtent = ctx.View().RenderExtent;
                cmd.SetViewport({0, 0}, renderExtent);
                cmd.SetScissor({0, 0}, renderExtent);
                registry.Bind(cmd);

                // Bind set 1 (the shadow system: both atlases, comparison sampler, and
                // both ring-buffered dynamic uniforms) and set 2 (the IBL maps + sampler,
                // always valid). The shadow rings are renderer-owned and framesInFlight-deep,
                // so their dynamic offset is the frame-in-flight index — not the shared
                // view-constants slot, which rings per viewport render. The IBL set has no
                // dynamic descriptors so the offsets still map to set 1's two in binding order.
                const u32 frameSlot = m_Context.GetCurrentFrameInFlight();
                cmd.BindDescriptorSets(DescriptorSetBindInfo{
                    .Sets = {shadowSet, iblSet},
                    .FirstSet = 1,
                    .PipelineBindPoint = PipelineBindPoint::Graphics,
                    .DynamicOffsets = {frameSlot * shadowRingStride,
                                       frameSlot * punctualRingStride},
                });

                // IBL is active when the resolved sky requests the IBL tier (iblAllowed)
                // AND its cube-backed source is resident — an environment map, or a baked
                // material sky whose material is loaded. A display-only source shows its
                // sky but does not light the scene. EnvIntensity rides the per-frame
                // SceneView; the sky itself is a separate pass.
                const SceneView& view = ctx.View();
                const bool sourceResident =
                    view.Environment.IsLoaded() || view.SkyMaterial.IsLoaded();
                const u32 iblEnabled = (iblAllowed && sourceResident) ? 1u : 0u;
                // The SH skylight is the second ambient arm, below IBL: active only when
                // its setting is on AND no environment is bound (IBL wins). The shader's
                // three-way branch reads it after IblEnabled.
                const u32 skylightOn = (skylight && iblEnabled == 0u) ? 1u : 0u;

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
                        .LightBase = registry.GetCurrentLightBase(),
                        .LightCount = view.LightCount,
                        .IblEnabled = iblEnabled,
                        .SkylightOn = skylightOn,
                        .PrefilterMipCount = prefilterMipCount,
                        .EnvIntensity = view.EnvironmentIntensity,
                        .SkylightIntensity = view.SkylightIntensity,
                        .LtcMatTexture = ltcMatHandle.Index,
                        .LtcMagTexture = ltcMagHandle.Index,
                        .AreaVertexBase = registry.GetCurrentAreaVertexBase(),
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
                        .LightBase = registry.GetCurrentLightBase(),
                        .LightCount = view.LightCount,
                        .IblEnabled = iblEnabled,
                        .SkylightOn = skylightOn,
                        .PrefilterMipCount = prefilterMipCount,
                        .EnvIntensity = view.EnvironmentIntensity,
                        .SkylightIntensity = view.SkylightIntensity,
                        .LtcMatTexture = ltcMatHandle.Index,
                        .LtcMagTexture = ltcMagHandle.Index,
                        .AreaVertexBase = registry.GetCurrentAreaVertexBase(),
                    });
                }
                cmd.DrawFullscreenTriangle();
            });
    }
}
