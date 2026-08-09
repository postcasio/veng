#include "SkyScenePass.h"

#include <fmt/format.h>

#include <Veng/Assert.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/PipelineLayout.h>
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
                        .FirstSet = 3,
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

    namespace
    {
        // Matches the SkyPushConstants block in Veng/sky.slang: the frame-folded material
        // selector, then the g-buffer depth handle, its sampler, and the current view-constants
        // region the sky fragment reconstructs the view ray from.
        struct SkyMaterialPushConstants
        {
            u32 MaterialIndex;
            u32 DepthTexture;
            u32 DepthSampler;
            u32 ViewConstantsIndex;
        };
    }

    SkyMaterialScenePass::SkyMaterialScenePass(Context& context, ResourceId targetId,
                                               ResourceId depthId, TextureHandle depthHandle,
                                               SamplerHandle samplerHandle, Format targetFormat,
                                               uvec2 extent)
        : m_Context(context), m_TargetId(targetId), m_DepthId(depthId), m_DepthHandle(depthHandle),
          m_SamplerHandle(samplerHandle), m_TargetFormat(targetFormat), m_Extent(extent)
    {
    }

    void SkyMaterialScenePass::SetMaterial(AssetHandle<MaterialInstance> material)
    {
        m_Material = std::move(material);
    }

    void SkyMaterialScenePass::Resize(const uvec2 extent)
    {
        m_Extent = extent;
    }

    void SkyMaterialScenePass::BuildPipeline()
    {
        VE_ASSERT(m_Material.IsLoaded(), "SkyMaterialScenePass: the Sky material is not resident");

        const MaterialInstance& material = *m_Material.Get();
        VE_ASSERT(material.GetDomain() == MaterialDomain::Sky,
                  "SkyMaterialScenePass: material '{}' is not a Sky material", material.GetName());

        // Only this scene-color-format-dependent pipeline is the pass's to build; the layout
        // (set 0 reserved, the sky push range) comes from the material loader.
        m_Pipeline = GraphicsPipeline::Create(
            m_Context,
            {
                .Name = fmt::format("Sky Material Pipeline ({})", material.GetName()),
                .ColorAttachments = {{.Format = m_TargetFormat}},
                .PipelineLayout = material.GetPipelineLayout(),
                .ShaderStages =
                    {
                        {.Stage = ShaderStage::Vertex, .Module = material.GetVertexModule()},
                        {.Stage = ShaderStage::Fragment, .Module = material.GetFragmentModule()},
                    },
            });
        m_PipelineMaterialId = m_Material.Id().Value;
    }

    void SkyMaterialScenePass::Declare(RenderGraph& graph, const PassIO& /*io*/)
    {
        // The pass is always contributed while the sky slot is enabled; the material is bound
        // per frame by the renderer and may arrive after this graph compiled (the consuming app
        // pushes it after world load), so the draw is gated in Execute, never the pass itself.
        // Contributing unconditionally keeps the Load/Store composite a no-op until a material
        // binds, rather than needing a recompile the moment one does.
        const TextureHandle depthHandle = m_DepthHandle;
        const SamplerHandle samplerHandle = m_SamplerHandle;

        graph
            .AddPass("Sky Material")
            // Composite over the lit scene color (preserve it; the shader discards foreground).
            .Color({
                .Resource = m_TargetId,
                .Load = LoadOp::Load,
                .Store = StoreOp::Store,
            })
            .Sample(m_DepthId)
            .Execute(
                [this, depthHandle, samplerHandle](PassContext& inner)
                {
                    // No material bound (yet) → contribute nothing; the Load/Store leaves the lit
                    // color untouched.
                    if (!m_Material.IsLoaded())
                    {
                        return;
                    }
                    // Build the scene-color-format pipeline on first use and when the bound
                    // material identity changes (a new sky authored/hot-reloaded) — the material
                    // may not have been resident when the graph compiled.
                    if (!m_Pipeline || m_PipelineMaterialId != m_Material.Id().Value)
                    {
                        BuildPipeline();
                    }

                    const ScenePassContext ctx = Wrap(inner);
                    CommandBuffer& cmd = ctx.Cmd();
                    const SceneView& view = ctx.View();
                    const MaterialInstance& material = *m_Material.Get();
                    const BindlessRegistry& registry = m_Context.GetBindlessRegistry();

                    const uvec2 renderExtent = view.RenderExtent;
                    cmd.BindPipeline(m_Pipeline);
                    cmd.SetViewport({0, 0}, renderExtent);
                    cmd.SetScissor({0, 0}, renderExtent);
                    registry.Bind(cmd);

                    // The material's own param writes (e.g. SetStorageBufferHandle) landed already;
                    // here the pass pushes the whole sky push block — the frame-folded selector plus
                    // the runtime depth handle/sampler/view-constants the material's contract reads.
                    cmd.PushConstants(SkyMaterialPushConstants{
                        .MaterialIndex = material.GetMaterialSelector(),
                        .DepthTexture = depthHandle.Index,
                        .DepthSampler = samplerHandle.Index,
                        .ViewConstantsIndex = registry.GetCurrentViewConstantsIndex(),
                    });
                    cmd.DrawFullscreenTriangle();
                });
    }
}
