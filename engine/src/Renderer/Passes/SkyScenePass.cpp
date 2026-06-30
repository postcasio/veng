#include "SkyScenePass.h"

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/SceneRenderer.h>

namespace Veng::Renderer
{
    namespace
    {
        // Matches atmosphere_sky.frag PushConstants byte-for-byte: four u32s, then the sun
        // direction (float3) + intensity (float), then the AtmosphereParams block — whose
        // trailing float3 pads to a 16-byte boundary exactly as the shader's struct does.
        struct SkyPushConstants
        {
            u32 DepthTexture;
            u32 Sampler;
            u32 ViewConstantsIndex;
            u32 Enabled;
            vec3 SunDirection;
            f32 Intensity;
            // AtmosphereParams (mirrors atmosphere_common.slang).
            vec3 RayleighScattering;
            f32 RayleighHeight;
            vec3 MieScattering;
            f32 MieExtinction;
            vec3 OzoneAbsorption;
            f32 MieHeight;
            f32 MieAnisotropy;
            f32 OzoneCenter;
            f32 OzoneWidth;
            f32 PlanetRadius;
            f32 AtmosphereRadius;
            f32 SunAngularRadius;
            f32 Pad0;
            f32 Pad1;
            vec3 SunIrradiance;
            f32 Pad2;
        };
    }

    SkyScenePass::SkyScenePass(Context& context, Ref<GraphicsPipeline> pipeline,
                               Ref<DescriptorSet> atmosphereSet, ResourceId targetId,
                               ResourceId depthId, TextureHandle depthHandle,
                               SamplerHandle samplerHandle, uvec2 extent)
        : m_Context(context), m_Pipeline(std::move(pipeline)),
          m_AtmosphereSet(std::move(atmosphereSet)), m_TargetId(targetId), m_DepthId(depthId),
          m_DepthHandle(depthHandle), m_SamplerHandle(samplerHandle), m_Extent(extent)
    {
    }

    void SkyScenePass::Resize(const uvec2 extent)
    {
        m_Extent = extent;
    }

    void SkyScenePass::Declare(RenderGraph& graph, const PassIO& /*io*/)
    {
        const TextureHandle depthHandle = m_DepthHandle;
        const SamplerHandle samplerHandle = m_SamplerHandle;
        const Ref<DescriptorSet> atmosphereSet = m_AtmosphereSet;

        graph
            .AddPass("Atmosphere Sky")
            // Composite over the lit scene color (preserve it; the shader discards foreground).
            .Color({
                .Resource = m_TargetId,
                .Load = LoadOp::Load,
                .Store = StoreOp::Store,
            })
            .Sample(m_DepthId)
            .Execute(
                [this, depthHandle, samplerHandle, atmosphereSet](PassContext& inner)
                {
                    const ScenePassContext ctx = Wrap(inner);
                    CommandBuffer& cmd = ctx.Cmd();
                    const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
                    const SceneView& view = ctx.View();

                    const uvec2 renderExtent = view.RenderExtent;
                    cmd.BindPipeline(m_Pipeline);
                    cmd.SetViewport({0, 0}, renderExtent);
                    cmd.SetScissor({0, 0}, renderExtent);
                    registry.Bind(cmd);

                    // The scattering + transmittance LUTs + sampler ride the atmosphere set (set 1).
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {atmosphereSet},
                        .FirstSet = 1,
                        .PipelineBindPoint = PipelineBindPoint::Graphics,
                    });

                    const Atmosphere& a = view.Atmosphere;
                    cmd.PushConstants(SkyPushConstants{
                        .DepthTexture = depthHandle.Index,
                        .Sampler = samplerHandle.Index,
                        .ViewConstantsIndex = registry.GetCurrentViewConstantsIndex(),
                        .Enabled = view.AtmosphereEnabled ? 1u : 0u,
                        .SunDirection = view.SunDirection,
                        .Intensity = view.AtmosphereIntensity,
                        .RayleighScattering = a.RayleighScattering,
                        .RayleighHeight = a.RayleighHeight,
                        .MieScattering = a.MieScattering,
                        .MieExtinction = a.MieExtinction,
                        .OzoneAbsorption = a.OzoneAbsorption,
                        .MieHeight = a.MieHeight,
                        .MieAnisotropy = a.MieAnisotropy,
                        .OzoneCenter = a.OzoneCenter,
                        .OzoneWidth = a.OzoneWidth,
                        .PlanetRadius = a.PlanetRadius,
                        .AtmosphereRadius = a.AtmosphereRadius,
                        .SunAngularRadius = a.SunAngularRadius,
                        .SunIrradiance = a.SunIrradiance,
                    });
                    cmd.DrawFullscreenTriangle();
                });
    }
}
