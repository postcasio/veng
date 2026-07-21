#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/ScenePass.h>

#include "AtmospherePrecompute.h"
#include "AutoExposureMeter.h"
#include "BloomPyramid.h"
#include "DebugBlitPipelines.h"
#include "DrawPlan.h"
#include "EnvironmentIbl.h"
#include "GpuBlocks.h"
#include "GpuCullSystem.h"
#include "PickingSystem.h"
#include "Passes/DebugBlitScenePasses.h"
#include "Passes/DebugDrawScenePass.h"
#include "Passes/DeferredLightingScenePass.h"
#include "Passes/GBufferScenePass.h"
#include "Passes/PickingScenePass.h"
#include "Passes/PointFieldScenePass.h"
#include "Passes/PunctualShadowScenePass.h"
#include "Passes/ShadowScenePass.h"
#include "Passes/SkyScenePass.h"
#include "Passes/SkyboxScenePass.h"
#include "Passes/SsaoScenePass.h"
#include "Passes/TaaScenePass.h"
#include "Passes/TranslucentScenePass.h"
#include "Passes/VolumeScenePass.h"
#include "ShadowSystem.h"
#include "RefractionGrab.h"
#include "SkyCubemapBake.h"
#include "SceneRendererIds.h"
#include "SkyResolver.h"
#include "DofChain.h"
#include "Passes/DofCompositeScenePass.h"
#include "SsrChain.h"
#include "TaaResolve.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/GBuffer.h>
#include <Veng/Renderer/HiZHistory.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/LightPacking.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/PunctualShadows.h>
#include <Veng/Renderer/ShadowCascades.h>
#include <Veng/Renderer/TaaJitter.h>
#include <Veng/Time.h>

#include <Veng/Math/AABB.h>
#include <Veng/Math/Ease.h>
#include <Veng/Math/Frustum.h>
#include <Veng/Math/SphericalHarmonics.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Environment.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Skeleton.h>

#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

namespace Veng::Renderer
{
    namespace
    {
        // Packs the set-1 ShadowConstants block from the fitted cascades: the tile-remapped
        // cascade view-projections, splits, texel sizes, depth ranges, and the ShadowParams
        // (inverse resolution, blend band, cascade count, enabled flag). shadowEnabled is set
        // only when the shadow pass is wired and a directional light exists this frame; otherwise
        // the lighting pass reads full visibility.
        ShadowConstantsBlock PackShadowConstants(const SceneRendererSettings& settings,
                                                 const CascadeData& cascades,
                                                 const bool shadowEnabled)
        {
            const ShadowAtlasGrid grid = ComputeShadowAtlasGrid(settings.CascadeCount);

            ShadowConstantsBlock shadowConstants{};
            for (u32 k = 0; k < cascades.Count && k < MaxCascades; ++k)
            {
                shadowConstants.CascadeViewProj[k] =
                    ComposeTileRemap(cascades.ViewProj[k], k, grid.Columns, grid.Rows);
                shadowConstants.CascadeSplits[k] = cascades.SplitFar[k];
                shadowConstants.CascadeTexelSize[k] = cascades.TexelWorldSize[k];
                shadowConstants.CascadeDepthRange[k] = cascades.DepthRange[k];
            }
            // Blend band: a view-space distance before each cascade's far split where the
            // lighting pass cross-fades into the next cascade. Sized from the first (smallest)
            // cascade so the band never exceeds any cascade's own range.
            const f32 firstSplit = cascades.Count > 0 ? cascades.SplitFar[0] : 0.0f;
            const f32 blendBand = firstSplit * 0.1f;

            shadowConstants.ShadowParams =
                vec4(1.0f / static_cast<f32>(settings.ShadowResolution), blendBand,
                     static_cast<f32>(cascades.Count), shadowEnabled ? 1.0f : 0.0f);
            return shadowConstants;
        }
    }

    // Kept out of the header so SceneRenderer.h needs no CompiledGraph definition.
    struct SceneRenderer::Internal
    {
        Unique<CompiledGraph> Graph;
        // The per-frame geometry submission plan PrepareDraws fills before each replay and
        // the geometry pass reads at record time (the pass holds a pointer to it).
        GBufferDrawPlan Plan;
        // The per-frame forward translucent submission plan PrepareDraws fills (back-to-front) and
        // the translucent pass reads at record time.
        TranslucentDrawPlan TranslucentPlan;
    };

    Unique<SceneRenderer> SceneRenderer::Create(const SceneRendererInfo& info)
    {
        return Unique<SceneRenderer>(new SceneRenderer(info));
    }

    SceneRenderer::SceneRenderer(const SceneRendererInfo& info)
        : m_Context(info.Context), m_Assets(info.Assets), m_OutputFormat(info.OutputFormat),
          m_Extent(info.Extent), m_ValidExtent(info.Extent), m_Settings(info.Settings),
          m_Internal(CreateUnique<Internal>())
    {
        ShadowSystem::ClampResolutions(m_Context, m_Settings);
        // Frames-in-flight is spine — the renderer's own rings (draw data, palette) size from it,
        // so seed it before those are allocated below. Each subsystem derives its own ring depth
        // from the context independently.
        m_FramesInFlight = m_Context.GetMaxFramesInFlight();
        m_Shadows = ShadowSystem::Create(m_Context, m_Settings);
        // The sky-resolve subsystem owns the IBL maps, the atmosphere LUTs, and the baked-sky cube;
        // their consumer set layouts must exist before CreatePipelines reserves sets (the lighting
        // layout reserves the IBL set, the sky layout the atmosphere set), so it is created here.
        m_SkyResolver = SkyResolver::Create(m_Context, m_Assets);
        // Bloom's down/up set layout is reserved by the SSR chain's blur pipeline layout, so bloom
        // is constructed before the SSR chain; its extent-sized pyramid is built by Resize below
        // (after the HDR target). TAA is grouped with it; both build their pipelines in their ctors.
        m_Bloom = BloomPyramid::Create(m_Context, m_Assets, m_Settings.Kernel);
        m_Taa = TaaResolve::Create(m_Context, m_Assets);
        m_Refraction = RefractionGrab::Create(m_Context, m_Assets);
        // The GPU cull subsystem owns the hi-Z reduce layouts the SSR chain's min-Z reduce borrows,
        // so it is constructed before the SSR chain (and resolves the active cull mode). Its hi-Z
        // reduce pipeline creation is not frame-observable, so building it here rather than in
        // CreatePipelines is the settled pipeline-order relaxation.
        m_GpuCull = GpuCullSystem::Create(m_Context, m_Assets, m_Settings);
        m_Picking = PickingSystem::Create(m_Context, m_Assets);
        CreatePipelines();
        // The SSR chain's blur pipeline layout reserves the bloom down/up set layout, and its
        // min-Z reduce pipeline builds on the GPU cull subsystem's hi-Z reduce layout — both must
        // already exist, so the chain is constructed after those subsystems and CreatePipelines.
        m_Ssr = SsrChain::Create(m_Context, m_Assets, m_GpuCull->GetHiZReduceLayout(),
                                 m_Bloom->GetDownUpSetLayout());
        // The depth-of-field chain's tile and fill pipeline layouts reserve the same bloom down/up
        // set layout, so it is likewise constructed after the bloom subsystem.
        m_Dof = DofChain::Create(m_Context, m_Assets, m_Bloom->GetDownUpSetLayout(), HdrFormat);

        CreateOutput();
        CreateGBuffer();
        CreateLtcResources();
        CreateCullResources();
        CreateHdr();
        m_Taa->Resize(m_Extent, m_Settings.TAA);
        // The pyramid's level-0 source and composite sets bind the fresh HDR view.
        m_Bloom->Resize(m_Extent, m_HdrView);
        // The min-Z reduce sets bind the fresh depth view from the g-buffer above.
        m_Ssr->Recreate(m_Settings, m_Extent, m_DepthView, m_GpuCull->GetHiZReduceSetLayout(),
                        m_Bloom->GetDownUpSetLayout());
        m_Dof->Recreate(m_Settings, m_Extent, m_HdrView, m_DepthView,
                        m_Bloom->GetDownUpSetLayout());
        m_Refraction->Recreate(m_Settings, m_Extent);
        // The metering set binds the HDR target, so the meter is created after CreateHdr.
        m_AutoExposure = AutoExposureMeter::Create(m_Context, m_Assets, m_HdrView);
        m_Picking->Recreate(m_Settings, m_Extent);
        Rebuild();
    }

    SceneRenderer::~SceneRenderer()
    {
        // Invariant: this hand-list holds only the spine handles the renderer registers directly —
        // the g-buffer channels, HDR, LTC LUTs, and the shared sampler. Every battery subsystem
        // (shadows, bloom, SSR, GPU cull, picking, sky, ...) owns and releases its own bindless
        // handles in its own destructor, so a new subsystem handle never has to be appended here.
        // Released before their images retire.
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_AlbedoHandle);
        bindless.Release(m_NormalHandle);
        bindless.Release(m_OrmHandle);
        bindless.Release(m_DepthHandle);
        bindless.Release(m_HdrHandle);
        bindless.Release(m_VelocityHandle);
        bindless.Release(m_EmissiveHandle);
        bindless.Release(m_LtcMatHandle);
        bindless.Release(m_LtcMagHandle);
        bindless.Release(m_SamplerHandle);
    }

    void SceneRenderer::Rebuild()
    {
        // Final is the full deferred chain; debug modes terminate after the g-buffer with one blit.
        // Bloom imports are declared only when active.
        //
        // Debug arms force-wire their producing battery pass so the visualized target
        // exists regardless of the corresponding Settings toggle.
        const bool debugBloom = m_Settings.Mode == DebugView::Bloom;
        const bool bloomActive =
            (m_Settings.Mode == DebugView::Final && m_Settings.Bloom) || debugBloom;
        m_BloomActive = bloomActive;

        // Auto-exposure meters the lit HDR in the Final path only (a debug arm has no tonemap tail
        // to drive). The enable edge re-snaps the adaptation so the image opens correctly exposed.
        const bool autoExposureActive =
            m_Settings.Mode == DebugView::Final && m_Settings.AutoExposure;
        if (autoExposureActive && !m_AutoExposureActive)
        {
            m_AutoExposure->RequestReset();
        }
        m_AutoExposureActive = autoExposureActive;

        // TAA is a Final-only resolve: it inserts the resolve + history-copy passes between
        // lighting and the tonemap tail and routes lighting into a separate lit target.
        const bool taaActive = m_Settings.Mode == DebugView::Final && m_Settings.TAA;
        m_TaaActive = taaActive;

        const bool debugShadow = m_Settings.Mode == DebugView::Shadows;
        const bool debugAo = m_Settings.Mode == DebugView::AO;
        // Cascades debug needs the shadow pass wired so cascade constants are written.
        const bool debugCascades = m_Settings.Mode == DebugView::Cascades;
        const bool debugPunctual = m_Settings.Mode == DebugView::PunctualShadows;

        // The Final view and the Bloom debug arm both composite the full scene before the bloom
        // tail, so both fold in the same contributors (shadows, SSAO, sky — plus the g-buffer
        // emissive channel the lighting pass adds) — the Bloom pyramid then blooms the same HDR the
        // Final view would.
        const bool sceneComposited = m_Settings.Mode == DebugView::Final || debugBloom;

        const bool shadowActive =
            (sceneComposited && m_Settings.Shadows) || debugShadow || debugCascades;
        m_ShadowActive = shadowActive;
        m_ShadowPass = nullptr;

        const bool punctualShadowActive =
            (sceneComposited && m_Settings.PunctualShadows) || debugPunctual;
        m_PunctualShadowActive = punctualShadowActive;
        m_PunctualShadowPass = nullptr;

        const bool ssaoFold = sceneComposited && m_Settings.AO;
        const bool ssaoActive = ssaoFold || debugAo;
        m_SsaoActive = ssaoActive;
        m_SsaoPass = nullptr;
        m_SkyMaterialPass = nullptr;

        // The sky pass topology is driven by the resolved Sky component, not a consumer toggle, and
        // reduces to one rule: a source fills the radiance cube (static) or composites direct
        // (dynamic), and the cube-backed sources share one display path. Every cube-backed source —
        // an environment (its own radiance cube), or a baked material/atmosphere (the bake cube) —
        // displays through the cubemap skybox pass; the two direct per-pixel passes
        // (SkyMaterialScenePass for a direct material, SkyScenePass for a direct atmosphere) survive
        // only as the authored dynamic modes. The SH skylight arm folds into the lighting pass for
        // any cube-backed source on the SH tier; m_SkylightActive gates the per-frame upload in
        // Execute.
        using SkySourceKind = SkyResolver::SkySourceKind;
        const SkySourceKind skyKind = m_SkyResolver->GetResolvedKind();
        const SkyLighting skyLighting = m_SkyResolver->GetResolvedLighting();
        const bool skyBaked = m_SkyResolver->IsResolvedBaked();
        const bool bakedSkyWanted =
            sceneComposited &&
            (skyKind == SkySourceKind::Material || skyKind == SkySourceKind::Atmosphere) &&
            skyBaked;
        const bool cubeBacked =
            (sceneComposited && skyKind == SkySourceKind::Environment) || bakedSkyWanted;
        const bool skyboxWanted = cubeBacked;
        const bool atmosphereWanted =
            sceneComposited && skyKind == SkySourceKind::Atmosphere && !skyBaked;
        const bool skyMaterialWanted =
            sceneComposited && skyKind == SkySourceKind::Material && !skyBaked;
        const bool skylightWanted = cubeBacked && skyLighting == SkyLighting::SH;
        m_SkyResolver->SetSkylightActive(skylightWanted);

        // The skybox pass samples the IBL radiance set for an environment sky, or the bake's
        // consumer set (same radiance binding) for a baked material/atmosphere sky.
        const Ref<DescriptorSet> skyboxSet = bakedSkyWanted ? m_SkyResolver->GetSkyBake().GetSet()
                                                            : m_SkyResolver->GetIbl().GetSet();

        // IBL lights the scene when the resolved sky is a cube-backed source on the IBL tier — an
        // environment (convolved from its equirect cube) or a baked material/atmosphere (convolved
        // from its bake cube). Either fills the IBL consumer set the lighting pass binds; a
        // display-only source (any other tier) shows its sky without lighting from it.
        const bool iblAllowed = cubeBacked && skyLighting == SkyLighting::IBL;

        // SSR is a Final-only effect plus its own debug arm; the debug arm force-wires the
        // trace so the raw reflection target is visible regardless of the Settings.SSR toggle.
        const bool debugReflections = m_Settings.Mode == DebugView::Reflections;
        const bool ssrActive =
            (m_Settings.Mode == DebugView::Final && m_Settings.SSR) || debugReflections;
        m_SsrActive = ssrActive;

        // Depth of field is a Final-only effect plus its own debug arm, the same gate shape. The
        // debug arm force-wires the chain with the feature off, but only its first two stages: the
        // gather, fill, and composite stay off, so inspecting the circle of confusion never alters
        // the HDR tail.
        const bool dofActive = (m_Settings.Mode == DebugView::Final && m_Settings.DepthOfField) ||
                               m_Settings.Mode == DebugView::CoC;
        const bool dofComposited = m_Settings.Mode == DebugView::Final && m_Settings.DepthOfField;
        m_DofActive = dofActive;
        m_DofComposited = dofComposited;

        // The scene-color copy runs wherever the translucent composite does (the Final view and
        // the Bloom debug arm), so a refractive material behaves identically in both.
        const bool refractionActive = sceneComposited && m_Settings.Refraction;
        m_RefractionActive = refractionActive;

        RenderGraph graph(m_Context);
        const ResourceId albedoId = graph.Import("SceneRenderer GBuffer Albedo");
        const ResourceId normalId = graph.Import("SceneRenderer GBuffer Normal");
        const ResourceId ormId = graph.Import("SceneRenderer GBuffer ORM");
        const ResourceId depthId = graph.Import("SceneRenderer GBuffer Depth");
        const ResourceId hdrId = graph.Import("SceneRenderer HDR");
        m_OutputId = graph.Import("SceneRenderer Output");

        m_AlbedoId = albedoId;
        m_NormalId = normalId;
        m_OrmId = ormId;
        m_DepthId = depthId;
        m_HdrId = hdrId;

        // When TAA is active the lighting pass writes a separate lit target the resolve
        // reads; the resolve then writes hdrId, so bloom/tonemap sample the resolved result.
        ResourceId litId{};
        ResourceId taaHistoryId{};
        ResourceId velocityId{};
        if (taaActive)
        {
            litId = graph.Import("SceneRenderer Lit");
            taaHistoryId = graph.Import("SceneRenderer TAA History");
        }
        // The velocity target (G3) is always imported — the g-buffer pass writes it on every
        // frame as a fourth color attachment.
        velocityId = graph.Import("SceneRenderer Velocity");
        // The emissive target (G4) is always imported — the g-buffer pass writes it on every
        // frame as a fifth color attachment.
        const ResourceId emissiveId = graph.Import("SceneRenderer GBuffer Emissive");
        m_LitId = litId;
        m_TaaHistoryId = taaHistoryId;
        m_VelocityId = velocityId;
        m_EmissiveId = emissiveId;

        // With SSR on, the lit scene color lands in an intermediate the SSR composite reflects
        // into before writing the HDR target — so SSR slots in exactly where TAA does, and the
        // bloom/tonemap tail still reads the HDR target unchanged.
        m_SsrSceneId = ResourceId{};
        if (ssrActive)
        {
            m_SsrSceneId = graph.Import("SceneRenderer SSR Scene");
            m_SsrReflectionChainId = graph.ImportImageMips("SceneRenderer SSR Reflection",
                                                           m_Ssr->GetReflectionMipCount());
            m_SsrHiZChainId =
                graph.ImportImageMips("SceneRenderer SSR MinZ", m_Ssr->GetHiZMipCount());
        }
        // Depth of field splices in the same way: the tail writes the chain's scene-color
        // intermediate and the composite writes the HDR target, so bloom, metering, and tonemap
        // read the id they always did and no extra copy appears.
        m_DofSceneId = ResourceId{};
        m_DofNearId = ResourceId{};
        m_DofFarId = ResourceId{};
        m_DofCocId = ResourceId{};
        m_DofTileId = ResourceId{};
        m_DofNearBlurId = ResourceId{};
        m_DofFarBlurId = ResourceId{};
        m_DofNearFillId = ResourceId{};
        m_DofFarFillId = ResourceId{};
        if (dofActive)
        {
            m_DofNearId = graph.Import("SceneRenderer DoF Near");
            m_DofFarId = graph.Import("SceneRenderer DoF Far");
            m_DofCocId = graph.Import("SceneRenderer DoF CoC");
            m_DofTileId = graph.Import("SceneRenderer DoF Tiles");
        }
        if (dofComposited)
        {
            m_DofSceneId = graph.Import("SceneRenderer DoF Scene");
            m_DofNearBlurId = graph.Import("SceneRenderer DoF Near Blur");
            m_DofFarBlurId = graph.Import("SceneRenderer DoF Far Blur");
            m_DofNearFillId = graph.Import("SceneRenderer DoF Near Fill");
            m_DofFarFillId = graph.Import("SceneRenderer DoF Far Fill");
        }

        // The id the HDR tail writes before the DoF composite hands the HDR target on.
        const ResourceId dofTargetId = dofComposited ? m_DofSceneId : hdrId;
        const ResourceId sceneColorId = ssrActive ? m_SsrSceneId : dofTargetId;
        const ResourceId lightingTargetId = taaActive ? litId : sceneColorId;

        // The refraction copy reads the same target the translucent pass blends over, whichever
        // intermediate the TAA/SSR routing picked; the handle is the bindless side of that id.
        m_RefractionSceneId = ResourceId{};
        m_RefractionDepthId = ResourceId{};
        if (refractionActive)
        {
            m_RefractionSceneId = graph.Import("SceneRenderer Refraction Scene");
            m_RefractionDepthId = graph.Import("SceneRenderer Refraction Depth");
        }
        const TextureHandle dofTargetHandle = dofComposited ? m_Dof->GetSceneHandle() : m_HdrHandle;
        const TextureHandle lightingTargetHandle =
            taaActive ? m_Taa->GetLitHandle()
                      : (ssrActive ? m_Ssr->GetSceneHandle() : dofTargetHandle);

        ResourceId shadowId{};
        if (shadowActive)
        {
            shadowId = graph.Import("SceneRenderer ShadowMap");
        }
        m_ShadowId = shadowId;

        ResourceId punctualShadowId{};
        if (punctualShadowActive)
        {
            punctualShadowId = graph.Import("SceneRenderer PunctualShadowMap");
        }
        m_PunctualShadowId = punctualShadowId;

        if (bloomActive)
        {
            // The pyramid imports one per-mip slot per level (the down/up sweep declares
            // per-level access on these); the result is a single full-resolution import.
            m_BloomChainId =
                graph.ImportImageMips("SceneRenderer Bloom Pyramid", m_Bloom->GetMipCount());
            m_BloomResultId = graph.Import("SceneRenderer Bloom Result");
        }

        m_AutoExposureId = ResourceId{};
        if (autoExposureActive)
        {
            m_AutoExposureId = graph.ImportBuffer("SceneRenderer AutoExposure");
        }

        TextureHandle ssaoHandle{};
        if (ssaoActive)
        {
            m_SsaoId = graph.Import("SceneRenderer SSAO");
        }

        m_Passes.clear();
        m_PointFieldPass.reset();
        m_DofCompositePass.reset();
        m_ScenePointFieldPass = nullptr;

        // The pass index the HDR tail (SSR composite, point fields, bloom sweep) is declared
        // before: the tonemap in the Final arm (set below), else the terminal pass.
        optional<usize> hdrTailAnchor;

        // The shadow pass runs first when active so the graph orders its write before
        // the lighting read.
        Ref<ImageView> shadowAtlasView;
        if (shadowActive)
        {
            auto shadowPass = CreateUnique<ShadowScenePass>(
                m_Context, m_Assets, m_Settings.ShadowResolution, m_Settings.CascadeCount);
            m_ShadowPass = shadowPass.get();
            shadowAtlasView = shadowPass->GetShadowView();
            m_Passes.push_back(std::move(shadowPass));
        }

        // Same ordering reason as the directional pass; the atlas is renderer-owned
        // (set 1 binding 4) and passed through PassIO.
        if (punctualShadowActive)
        {
            auto punctualPass = CreateUnique<PunctualShadowScenePass>(
                m_Context, m_Assets, m_Settings.PunctualShadowResolution);
            m_PunctualShadowPass = punctualPass.get();
            m_Passes.push_back(std::move(punctualPass));
        }

        // Recreate the shadow sets against the wired atlas, or the dummy when shadows
        // are off, before the passes below copy their Refs.
        m_Shadows->RebuildSets(shadowActive ? shadowAtlasView : m_Shadows->GetDummyView());

        // The GPU cull arm imports the indirect command buffer so the cull compute pass
        // (StorageBufferWrite) and the geometry pass (IndirectRead) share it through the
        // graph-derived buffer barrier.
        const ResourceId indirectId = m_GpuCull->ImportIndirect(graph);

        auto gbufferPass = CreateUnique<GBufferScenePass>(m_Context, m_Extent, &m_Internal->Plan,
                                                          m_GpuCull->GetActiveCull(), indirectId);
        m_Passes.push_back(std::move(gbufferPass));

        // The entity-id picking pass: a depth-tested re-draw of the same survivors into the R32Uint
        // EntityId target through its own RenderingInfo (the shipping g-buffer pass above untouched).
        // Allocated and wired only when Picking is set (targets exist), so the shipping path is unchanged.
        if (m_Picking->IsAllocated())
        {
            const ResourceId entityIdId = graph.Import("SceneRenderer EntityId");
            const ResourceId pickingDepthId = graph.Import("SceneRenderer Picking Depth");
            m_Picking->SetGraphIds(entityIdId, pickingDepthId);
            m_Passes.push_back(CreateUnique<PickingScenePass>(
                m_Context, m_Extent, &m_Internal->Plan, m_Picking->GetStaticPipelinePointer(),
                m_Picking->GetSkinnedPipelinePointer(), entityIdId, pickingDepthId));

            // The billboard id-write runs immediately after the mesh id pass — still in the
            // geometry-pass timeframe, while the EntityId target is bound — writing each pickable
            // billboard's owning entity id over a min-size proxy footprint, hardware-depth-discarded
            // against the mesh depth the picking pass just stored. Decorative billboards (PickId 0)
            // are untouched here; the DebugDrawScenePass still draws them after tonemap unchanged.
            m_Passes.push_back(CreateUnique<BillboardPickScenePass>(
                m_Context, m_Assets, &m_DebugDraw, GBuffer::DepthFormat,
                m_Context.GetMaxFramesInFlight(), m_Extent, entityIdId, pickingDepthId));
        }
        else
        {
            m_Picking->SetGraphIds(ResourceId{}, ResourceId{});
        }

        // Created before the tail switch so ssaoHandle is set when the Final arm reads it.
        if (ssaoActive)
        {
            auto ssaoPass =
                CreateUnique<SsaoScenePass>(m_Context, m_SsaoPipeline, m_SamplerHandle, m_Extent);
            m_SsaoPass = ssaoPass.get();
            ssaoHandle = ssaoPass->GetAoHandle();
            m_Passes.push_back(std::move(ssaoPass));
        }

        switch (m_Settings.Mode)
        {
        case DebugView::Final:
        {
            m_Passes.push_back(CreateUnique<DeferredLightingScenePass>(
                m_Context, ssaoFold ? m_SsaoLightingPipeline : m_LightingPipeline, m_Extent,
                ssaoFold, m_Shadows->GetSet(), m_Shadows->GetConstantsRingStride(),
                m_Shadows->GetPunctualRingStride(), m_SkyResolver->GetIbl().GetSet(),
                m_SkyResolver->GetIbl().GetPrefilterMipCount(), skylightWanted, iblAllowed));

            // The resolved sky source wires exactly one fullscreen sky pass in the shared sky slot,
            // before the TAA/SSR/bloom tail so the sky resolves, reflects, and tonemaps with the
            // scene. An environment source samples its radiance cube; an atmosphere source samples
            // the procedural LUTs; a material source runs the authored Sky-domain material.
            if (skyboxWanted)
            {
                m_Passes.push_back(CreateUnique<SkyboxScenePass>(
                    m_Context, m_SkyboxPipeline, skyboxSet, lightingTargetId, depthId,
                    m_DepthHandle, m_SamplerHandle, m_Extent, bakedSkyWanted));
            }
            if (atmosphereWanted)
            {
                m_Passes.push_back(CreateUnique<SkyScenePass>(
                    m_Context, m_SkyPipeline, m_SkyResolver->GetAtmosphere().GetSet(),
                    lightingTargetId, depthId, m_DepthHandle, m_SamplerHandle, m_Extent));
            }
            if (skyMaterialWanted)
            {
                auto skyMaterialPass = CreateUnique<SkyMaterialScenePass>(
                    m_Context, lightingTargetId, depthId, m_DepthHandle, m_SamplerHandle, HdrFormat,
                    m_Extent);
                m_SkyMaterialPass = skyMaterialPass.get();
                m_Passes.push_back(std::move(skyMaterialPass));
            }

            // SceneColor-placed point fields accumulate into the lit scene color here — after the
            // sky composite, ahead of the refraction copy and the translucent pass —
            // so translucents blend over the fields and a refractive material's grab includes them.
            // The in-list io.Hdr is exactly the lit target this position writes.
            if (m_ScenePointFieldActive)
            {
                auto scenePointFields = CreateUnique<PointFieldScenePass>(
                    m_Context, m_Assets, &m_ScenePointFields, HdrFormat, m_SamplerHandle,
                    m_Context.GetMaxFramesInFlight());
                scenePointFields->SetForceDirect(m_PointFieldForceDirect);
                m_ScenePointFieldPass = scenePointFields.get();
                m_Passes.push_back(std::move(scenePointFields));
            }

            // Volume fields ray-march into the lit scene color in the same slot — after the sky
            // composite, ahead of the refraction copy and the translucent pass — so a field
            // attenuates the backdrop behind it, the refraction grab captures it, translucents blend
            // over it, and TAA resolves the per-pixel march jitter. The in-list io.Hdr is exactly the
            // lit target this position writes.
            if (m_VolumeFieldActive)
            {
                m_Passes.push_back(CreateUnique<VolumeScenePass>(
                    m_Context, m_Assets, &m_VolumeFields, HdrFormat, m_SamplerHandle));
            }

            // Forward translucent draws alpha-blend into the lit scene color after the sky
            // composite and before the TAA/bloom/tonemap tail, so translucents resolve,
            // bloom, and tonemap with the scene. Depth-tested against the opaque depth, depth-write
            // off, sorted back-to-front. Additive to the pipeline: no toggle — a scene with no
            // translucent submesh records an empty pass. With Refraction on, the copy pass grabs
            // the lit scene color first so a translucent fragment can sample the scene behind it.
            if (refractionActive)
            {
                m_Refraction->Declare(m_Passes, lightingTargetId, lightingTargetHandle, depthId,
                                      m_DepthHandle, m_RefractionSceneId, m_RefractionDepthId,
                                      m_SamplerHandle, m_Extent);
            }
            m_Passes.push_back(CreateUnique<TranslucentScenePass>(
                m_Context, m_Extent, &m_Internal->TranslucentPlan, lightingTargetId, depthId,
                m_RefractionSceneId, m_RefractionDepthId, HdrFormat));

            // TAA resolves the lit target into the HDR target the tail samples, so it sits
            // between lighting and the bloom/tonemap tail.
            if (taaActive)
            {
                // The resolve writes the scene-color target SSR reads (the HDR target directly
                // when SSR is off); the SSR composite then writes the HDR target.
                m_Passes.push_back(CreateUnique<TaaScenePass>(
                    m_Context, m_Taa->GetResolvePipeline(), m_Taa->GetCopyPipeline(), litId,
                    taaHistoryId, sceneColorId, depthId, velocityId, m_Taa->GetLitHandle(),
                    m_Taa->GetHistoryHandle(), m_VelocityHandle, m_Taa->GetHistoryResetPointer(),
                    m_Extent));
            }

            // The point-field pass accumulates the scene's live fields into the final HDR color
            // as part of the HDR tail — after the TAA resolve and SSR composite, before the bloom
            // sweep — so the fields' additive radiance rolls off through the tone curve and
            // blooms. Declared at the tail anchor (not pushed here), since only the anchor point
            // sits between the SSR composite and the bloom read of the final HDR target.
            if (m_PointFieldActive)
            {
                m_PointFieldPass = CreateUnique<PointFieldScenePass>(
                    m_Context, m_Assets, &m_PointFields, HdrFormat, m_SamplerHandle,
                    m_Context.GetMaxFramesInFlight());
                static_cast<PointFieldScenePass*>(m_PointFieldPass.get())
                    ->SetForceDirect(m_PointFieldForceDirect);
            }

            // The composite is declared at the tail anchor (between the point fields and the bloom
            // read of the finished HDR), so like the point-field pass it is held outside m_Passes.
            if (dofComposited)
            {
                m_DofCompositePass = CreateUnique<DofCompositeScenePass>(
                    m_Context, m_Dof->GetCompositePipeline(), m_DofNearFillId,
                    m_Dof->GetNearFillHandle(), m_DofFarFillId, m_Dof->GetFarFillHandle(), m_HdrId,
                    m_Dof->GetSamplerHandle(), m_Dof->GetHalfExtent(), m_Extent);
            }

            // Tonemap source: bloom composite when bloom is on, raw HDR otherwise.
            ResourceId tonemapSourceId = m_HdrId;
            TextureHandle tonemapSourceHandle = m_HdrHandle;

            if (bloomActive)
            {
                // The bloom down/up/composite compute sweep is declared into the graph by
                // the tail anchor in the pass loop; here the tonemap just reads its result.
                tonemapSourceId = m_BloomResultId;
                tonemapSourceHandle = m_Bloom->GetResultHandle();
            }

            // The HDR tail declares just before the tonemap — never after it, even when a
            // post-tonemap pass (DebugDraw) follows in the list.
            hdrTailAnchor = m_Passes.size();
            m_Passes.push_back(
                CreateUnique<PostProcessScenePass>(m_Context, m_TonemapMaterial,
                                                   PostProcessInput{
                                                       .Source = tonemapSourceId,
                                                       .SourceTexture = tonemapSourceHandle,
                                                       .Sampler = m_SamplerHandle,
                                                       .TextureField = "Hdr",
                                                       .SamplerField = "HdrSampler",
                                                   },
                                                   m_OutputId, m_OutputFormat, m_Extent));

            // Debug-draw composites the accumulator over the tonemapped LDR scene color, after
            // the terminal tonemap, so gizmos render at native resolution with exact colors. It
            // samples the g-buffer depth for the occluded fade (no hardware depth test).
            if (m_Settings.DebugDraw)
            {
                m_Passes.push_back(CreateUnique<DebugDrawScenePass>(
                    m_Context, m_Assets, &m_DebugDraw, m_OutputFormat, m_SamplerHandle,
                    m_Context.GetMaxFramesInFlight(), m_Extent));
            }

            break;
        }
        case DebugView::Albedo:
            m_Passes.push_back(
                CreateUnique<FullscreenBlitScenePass>(m_Context, m_DebugBlits->Albedo, m_Extent,
                                                      FullscreenBlitScenePass::Source::Albedo));
            break;
        case DebugView::Normal:
            m_Passes.push_back(
                CreateUnique<FullscreenBlitScenePass>(m_Context, m_DebugBlits->Normal, m_Extent,
                                                      FullscreenBlitScenePass::Source::Normal));
            break;
        case DebugView::Depth:
            m_Passes.push_back(CreateUnique<FullscreenBlitScenePass>(
                m_Context, m_DebugBlits->Depth, m_Extent, FullscreenBlitScenePass::Source::Depth));
            break;
        case DebugView::Occlusion:
            m_Passes.push_back(CreateUnique<OrmBlitScenePass>(m_Context, m_DebugBlits->Orm,
                                                              m_Extent, /*channel=*/0));
            break;
        case DebugView::Roughness:
            m_Passes.push_back(CreateUnique<OrmBlitScenePass>(m_Context, m_DebugBlits->Orm,
                                                              m_Extent, /*channel=*/1));
            break;
        case DebugView::Metallic:
            m_Passes.push_back(CreateUnique<OrmBlitScenePass>(m_Context, m_DebugBlits->Orm,
                                                              m_Extent, /*channel=*/2));
            break;
        case DebugView::AO:
            m_Passes.push_back(CreateUnique<FullscreenBlitScenePass>(
                m_Context, m_DebugBlits->Ao, m_Extent, FullscreenBlitScenePass::Source::Ao));
            break;
        case DebugView::Shadows:
            // Reads the cascade atlas through the dedicated set (raw depth), not bindless.
            m_Passes.push_back(CreateUnique<ShadowBlitScenePass>(
                m_Context, m_DebugBlits->Shadow, m_Extent, m_Shadows->GetBlitSet(),
                ShadowBlitScenePass::Source::Directional));
            break;
        case DebugView::PunctualShadows:
            // Reads the punctual atlas through the dedicated set; binding 0 is
            // rewritten below after the pass set is chosen.
            m_Passes.push_back(CreateUnique<ShadowBlitScenePass>(
                m_Context, m_DebugBlits->Shadow, m_Extent, m_Shadows->GetBlitSet(),
                ShadowBlitScenePass::Source::Punctual));
            break;
        case DebugView::Cascades:
            // Tints fragments by cascade selection and writes the output directly (no tonemap tail).
            m_Passes.push_back(CreateUnique<DeferredLightingScenePass>(
                m_Context, m_CascadeDebugPipeline, m_Extent, /*useSsao=*/false, m_Shadows->GetSet(),
                m_Shadows->GetConstantsRingStride(), m_Shadows->GetPunctualRingStride(),
                m_SkyResolver->GetIbl().GetSet(), m_SkyResolver->GetIbl().GetPrefilterMipCount(),
                skylightWanted, iblAllowed,
                /*writeToOutput=*/true));
            break;
        case DebugView::Bloom:
            // Bloom samples the composited HDR, so the same contributors the Final arm folds into it
            // run first — lighting (which adds the g-buffer emissive channel), then the
            // sky/atmosphere composites (each gated on its own toggle, writing the lit target before
            // the tail) — so the pyramid blooms the scene the Final view blooms. The force-wired
            // bloom sweep (BloomPyramid::Declare, after the last pass)
            // writes the pyramid, and the terminal blit shows mip 0 after the up-sweep — the
            // accumulated bloom before composite.
            m_Passes.push_back(CreateUnique<DeferredLightingScenePass>(
                m_Context, ssaoFold ? m_SsaoLightingPipeline : m_LightingPipeline, m_Extent,
                ssaoFold, m_Shadows->GetSet(), m_Shadows->GetConstantsRingStride(),
                m_Shadows->GetPunctualRingStride(), m_SkyResolver->GetIbl().GetSet(),
                m_SkyResolver->GetIbl().GetPrefilterMipCount(), skylightWanted, iblAllowed));
            if (skyboxWanted)
            {
                m_Passes.push_back(CreateUnique<SkyboxScenePass>(
                    m_Context, m_SkyboxPipeline, skyboxSet, lightingTargetId, depthId,
                    m_DepthHandle, m_SamplerHandle, m_Extent, bakedSkyWanted));
            }
            if (atmosphereWanted)
            {
                m_Passes.push_back(CreateUnique<SkyScenePass>(
                    m_Context, m_SkyPipeline, m_SkyResolver->GetAtmosphere().GetSet(),
                    lightingTargetId, depthId, m_DepthHandle, m_SamplerHandle, m_Extent));
            }
            if (skyMaterialWanted)
            {
                auto skyMaterialPass = CreateUnique<SkyMaterialScenePass>(
                    m_Context, lightingTargetId, depthId, m_DepthHandle, m_SamplerHandle, HdrFormat,
                    m_Extent);
                m_SkyMaterialPass = skyMaterialPass.get();
                m_Passes.push_back(std::move(skyMaterialPass));
            }
            // The same forward translucent composite the Final arm folds into the lit target, so
            // the pyramid blooms the scene the Final view blooms — including the refraction grab.
            if (refractionActive)
            {
                m_Refraction->Declare(m_Passes, lightingTargetId, lightingTargetHandle, depthId,
                                      m_DepthHandle, m_RefractionSceneId, m_RefractionDepthId,
                                      m_SamplerHandle, m_Extent);
            }
            m_Passes.push_back(CreateUnique<TranslucentScenePass>(
                m_Context, m_Extent, &m_Internal->TranslucentPlan, lightingTargetId, depthId,
                m_RefractionSceneId, m_RefractionDepthId, HdrFormat));
            m_Passes.push_back(CreateUnique<FullscreenBlitScenePass>(
                m_Context, m_DebugBlits->Albedo, m_Extent, FullscreenBlitScenePass::Source::Bloom));
            break;
        case DebugView::MotionVectors:
            // The g-buffer pass writes the velocity target (G3); this blit colorizes it as an
            // optical-flow field.
            m_Passes.push_back(CreateUnique<FullscreenBlitScenePass>(
                m_Context, m_DebugBlits->Motion, m_Extent,
                FullscreenBlitScenePass::Source::MotionVectors));
            break;
        case DebugView::Reflections:
            // Lighting writes the scene-color intermediate the force-wired SSR trace reflects
            // (DeclareSsr, before this blit); the blit shows the raw reflection target.
            m_Passes.push_back(CreateUnique<DeferredLightingScenePass>(
                m_Context, m_LightingPipeline, m_Extent, /*useSsao=*/false, m_Shadows->GetSet(),
                m_Shadows->GetConstantsRingStride(), m_Shadows->GetPunctualRingStride(),
                m_SkyResolver->GetIbl().GetSet(), m_SkyResolver->GetIbl().GetPrefilterMipCount(),
                skylightWanted, iblAllowed));
            m_Passes.push_back(CreateUnique<FullscreenBlitScenePass>(
                m_Context, m_DebugBlits->Albedo, m_Extent,
                FullscreenBlitScenePass::Source::Reflections));
            break;
        case DebugView::CoC:
            // The chain's prefilter reads the lit HDR, so lighting is force-wired here as the
            // Reflections arm force-wires it; the force-wired prefilter and tile dilation then
            // write the signed-radius target this blit shows as a near/far ramp.
            m_Passes.push_back(CreateUnique<DeferredLightingScenePass>(
                m_Context, m_LightingPipeline, m_Extent, /*useSsao=*/false, m_Shadows->GetSet(),
                m_Shadows->GetConstantsRingStride(), m_Shadows->GetPunctualRingStride(),
                m_SkyResolver->GetIbl().GetSet(), m_SkyResolver->GetIbl().GetPrefilterMipCount(),
                skylightWanted, iblAllowed));
            m_Passes.push_back(
                CreateUnique<CocBlitScenePass>(m_Context, m_DebugBlits->Coc, m_Extent));
            break;
        case DebugView::Emissive:
            // The g-buffer pass writes the emissive channel (G4); this blit shows the authored
            // emissive contribution alone, the channel inspectable like every other g-buffer arm.
            m_Passes.push_back(
                CreateUnique<FullscreenBlitScenePass>(m_Context, m_DebugBlits->Albedo, m_Extent,
                                                      FullscreenBlitScenePass::Source::Emissive));
            break;
        }

        // Point binding 0 at the punctual atlas for the debug blit (overwrites the
        // cascade/dummy atlas written above — valid in place, the set is fresh this
        // rebuild and not yet bound).
        if (debugPunctual)
        {
            m_Shadows->GetBlitSet()->Write(0, m_Shadows->GetPunctualView());
        }

        const PassIO io{
            .GBufferAlbedo = albedoId,
            .GBufferNormal = normalId,
            .GBufferOrm = ormId,
            .GBufferDepth = depthId,
            .AlbedoHandle = m_AlbedoHandle,
            .NormalHandle = m_NormalHandle,
            .OrmHandle = m_OrmHandle,
            .DepthHandle = m_DepthHandle,
            .Hdr = lightingTargetId,
            .HdrHandle = m_HdrHandle,
            .Ssao = m_SsaoId,
            .SsaoHandle = ssaoHandle,
            .BloomMip0 = bloomActive ? m_BloomChainId.Level(0) : ResourceId{},
            .BloomMip0Handle = m_Bloom->GetMip0Handle(),
            .Velocity = velocityId,
            .VelocityHandle = m_VelocityHandle,
            .GBufferEmissive = emissiveId,
            .EmissiveHandle = m_EmissiveHandle,
            .SsrReflection = ssrActive ? m_SsrReflectionChainId.Level(0) : ResourceId{},
            .SsrReflectionHandle = m_Ssr->GetReflectionSampleHandle(),
            .DofCoc = m_DofCocId,
            .DofCocHandle = m_Dof->GetCocHandle(),
            .SamplerHandle = m_SamplerHandle,
            .LtcMatHandle = m_LtcMatHandle,
            .LtcMagHandle = m_LtcMagHandle,
            .ShadowMap = shadowId,
            .ShadowView = shadowAtlasView,
            .PunctualShadowMap = punctualShadowId,
            .PunctualShadowView = m_Shadows->GetPunctualView(),
            .Output = m_OutputId,
        };

        // Import the hi-Z chain once: the GPU cull samples last frame's pyramid and the reduction
        // at the tail writes this frame's pyramid into the same slots.
        m_GpuCull->ImportHiZChain(graph);

        // The GPU cull compute pass must precede the geometry pass: it writes the
        // indirect commands the geometry pass reads. Declared before the pass loop so it
        // is earlier in the graph's declaration (execution) order than the g-buffer pass.
        if (m_GpuCull->GetActiveCull() == SceneRendererSettings::CullMode::GPU)
        {
            m_GpuCull->DeclareCull(graph, &m_Internal->Plan);
        }

        // The HDR tail — SSR composite, point fields, bloom sweep — is declared just before the
        // pass that consumes the final HDR: the tonemap in the Final arm, the terminal blit in a
        // debug arm. Anchoring on that pass (not the list's last) keeps the tail ahead of the
        // tonemap even when a post-tonemap pass (DebugDraw) follows, since execution follows
        // declaration order.
        const usize tailAnchor = hdrTailAnchor.value_or(m_Passes.size() - 1);
        for (usize i = 0; i < m_Passes.size(); ++i)
        {
            const Unique<ScenePass>& pass = m_Passes[i];
            pass->Configure(m_Settings);
            pass->Resize(m_Extent);

            if (i == tailAnchor)
            {
                // SSR composes the reflected scene color into the HDR target; the point fields
                // accumulate over that; the bloom sweep then reads the finished HDR.
                if (ssrActive)
                {
                    m_Ssr->Declare(graph, m_SsrSceneId, m_SsrReflectionChainId, m_SsrHiZChainId,
                                   m_NormalId, m_OrmId, m_DepthId, dofTargetId, m_DepthHandle,
                                   m_NormalHandle, m_OrmHandle, m_AlbedoHandle, m_SamplerHandle);
                }
                if (m_PointFieldPass != nullptr)
                {
                    // The fields write the last scene-color target of the tail, not the pre-TAA/SSR
                    // lit target the in-list passes see as io.Hdr.
                    PassIO fieldIo = io;
                    fieldIo.Hdr = dofTargetId;
                    m_PointFieldPass->Declare(graph, fieldIo);
                }
                if (dofActive)
                {
                    // The debug arm has no scene intermediate, so its prefilter reads the HDR
                    // target directly — matching the source view its descriptor set bound.
                    m_Dof->Declare(graph, dofComposited ? m_DofSceneId : m_HdrId, m_DepthId,
                                   m_DofNearId, m_DofFarId, m_DofCocId, m_DofTileId,
                                   m_DofNearBlurId, m_DofFarBlurId, m_DofNearFillId, m_DofFarFillId,
                                   /*stagesOnly=*/!dofComposited);
                }
                if (m_DofCompositePass != nullptr)
                {
                    PassIO dofIo = io;
                    dofIo.Hdr = m_DofSceneId;
                    dofIo.HdrHandle = m_Dof->GetSceneHandle();
                    m_DofCompositePass->Declare(graph, dofIo);
                }
                if (bloomActive)
                {
                    m_Bloom->Declare(graph, m_HdrId, m_BloomChainId, m_BloomResultId,
                                     *m_AutoExposure);
                }
                if (autoExposureActive)
                {
                    m_AutoExposure->Declare(graph, m_HdrId, m_AutoExposureId, m_Extent);
                }
            }

            pass->Declare(graph, io);
        }

        // The hi-Z reduction runs last so it reduces this frame's completed depth.
        // Nothing samples the pyramid yet — it is built and persisted for the
        // next-frame occlusion test — so it changes no rendered pixel.
        m_GpuCull->DeclareHiZReduction(graph, m_DepthId);

        m_Internal->Graph = graph.Compile();
    }

    void SceneRenderer::ResolvePointFields(const SceneView& view)
    {
        // Refill this Execute's live field sets from the scene's PointField components — the lights
        // model, split by each component's authored Placement. Each component's authored Lod rides
        // its built field so the pass reads the authored knobs; a null or empty Field contributes
        // nothing.
        m_PointFields.clear();
        m_ScenePointFields.clear();
        for (auto [entity, field] : view.World.View<Veng::PointField>())
        {
            const Ref<Renderer::PointField>& built = field.Field;
            if (built == nullptr || built->GetPointCount() == 0)
            {
                continue;
            }
            built->SetLod(field.Lod);
            if (field.Placement == Renderer::PointFieldPlacement::SceneColor)
            {
                m_ScenePointFields.push_back(built.get());
            }
            else
            {
                m_PointFields.push_back(built.get());
            }
        }

        // Presence drives each pass: insert it the first frame a live field exists in its
        // placement, drop it when the last one goes. The recompile happens at this frame boundary
        // and reuses the imported output (identity preserved), so a cached GetOutput() ref stays
        // valid.
        const bool active = !m_PointFields.empty();
        const bool sceneActive = !m_ScenePointFields.empty();
        if (active != m_PointFieldActive || sceneActive != m_ScenePointFieldActive)
        {
            m_PointFieldActive = active;
            m_ScenePointFieldActive = sceneActive;
            Rebuild();
        }
    }

    void SceneRenderer::ResolveVolumeFields(const SceneView& view)
    {
        // Refill this Execute's live field set from the scene's VolumeField components — the lights
        // model. Each component's authored knobs fold into the instance beside its built field; a
        // null Field contributes nothing.
        m_VolumeFields.clear();
        for (auto [entity, field] : view.World.View<Veng::VolumeField>())
        {
            if (field.Field == nullptr)
            {
                continue;
            }
            m_VolumeFields.push_back(VolumeFieldInstance{
                .Field = field.Field.get(),
                .Opacity = field.Opacity,
                .EmissionScale = field.EmissionScale,
                .ExtinctionScale = field.ExtinctionScale,
                .Steps = field.Steps,
            });
        }

        // Presence drives the pass: insert it the first frame a live field exists, drop it when the
        // last one goes. The recompile happens at this frame boundary and reuses the imported output
        // (identity preserved), so a cached GetOutput() ref stays valid.
        const bool active = !m_VolumeFields.empty();
        if (active != m_VolumeFieldActive)
        {
            m_VolumeFieldActive = active;
            Rebuild();
        }
    }

    void SceneRenderer::PrepareDraws(const SceneView& view, const u32 viewConstantsIndex)
    {
        GBufferDrawPlan& plan = m_Internal->Plan;
        plan.Cull = m_GpuCull->GetActiveCull();
        plan.DrawDataSet = m_DrawDataSet;
        plan.CandidateIdBuffer = m_CandidateIdBuffer;
        plan.IndirectBuffer = m_GpuCull->GetIndirectBuffer();
        plan.PipelineMaterial = nullptr;
        plan.Slots.clear();
        plan.Groups.clear();
        plan.SkinnedPipelineMaterial = nullptr;
        plan.PaletteSet = m_PaletteSet;
        plan.SkinnedSlots.clear();
        plan.SkinnedGroups.clear();
        m_PaletteBaseByEntity.clear();

        TranslucentDrawPlan& translucentPlan = m_Internal->TranslucentPlan;
        translucentPlan.DrawDataSet = m_DrawDataSet;
        translucentPlan.CandidateIdBuffer = m_CandidateIdBuffer;
        translucentPlan.Draws.clear();

        // The DrawData / candidate / palette / indirect buffers are renderer-owned and
        // framesInFlight-deep, so they ring by the frame-in-flight index; only the shared
        // view-constants push (viewConstantsIndex) rings per viewport render.
        const u32 frameIndex = m_Context.GetCurrentFrameInFlight();
        const u32 frameBase = frameIndex * MaxCullCandidates;
        const u32 paletteRegionBase = frameIndex * MaxSkinningMatricesPerFrame;
        u32 paletteCursor = 0;
        auto* paletteData = static_cast<mat4*>(m_PaletteBuffer->GetMappedData());
        plan.Push = SurfacePush{.FrameBase = frameBase, .ViewConstantsIndex = viewConstantsIndex};
        translucentPlan.Push = plan.Push;
        plan.IndirectRegionOffset =
            frameIndex * MaxCullCandidates * static_cast<u32>(sizeof(DrawIndexedIndirectCommand));

        const std::span<const SubMeshCandidate> candidates =
            view.Broadphase->GetSubMeshCandidates();

        // The camera-frustum survivors, in ascending Cull-id order; the upload source for
        // both modes (the GPU compute adds only occlusion, never re-running the frustum).
        m_CullScratch.clear();
        if (m_Settings.FrustumCull)
        {
            const Frustum cameraFrustum = Frustum::FromViewProjection(view.Camera.ViewProjection());
            view.Broadphase->Cull(cameraFrustum, m_CullScratch);
        }
        else
        {
            m_CullScratch.reserve(candidates.size());
            for (u32 i = 0; i < candidates.size(); ++i)
            {
                m_CullScratch.push_back(i);
            }
        }

        // The per-submesh frustum survivors are both the frustum-survived and the drawn funnel
        // stage: a survivor is a draw under CullMode::CPU (a materialless or not-yet-resident one
        // counts even though it fills no slot below — the per-submesh cull result the draw-count
        // fixtures assert on). The occlusion stage shows up separately as GetLastGpuSurvivorCount.
        m_FrustumSurvivedCount = static_cast<u32>(m_CullScratch.size());
        m_LastDrawnCount = m_FrustumSurvivedCount;

        // Per-object velocity needs each drawn entity's previous-frame world matrix. Record
        // this frame's worlds (swapped to "previous" after Execute) and look the prior one up
        // when filling DrawData. The surface pass writes velocity every frame (G3), so this is
        // always maintained.
        const auto PackEntity = [](Entity e) -> u64
        { return (static_cast<u64>(e.Index) << 32) | static_cast<u64>(e.Generation); };
        {
            m_CurrentWorlds.clear();
            for (const VisibleMesh& vm : view.Visible)
            {
                m_CurrentWorlds[PackEntity(vm.Owner)] = vm.World;
            }
        }

        auto* drawData = static_cast<GpuDrawData*>(m_DrawDataBuffer->GetMappedData());
        // The GPU cull candidate span for this frame's ring region (null under CullMode::CPU); the
        // renderer-coordinated write surface into the subsystem-owned candidate buffer.
        GpuCullCandidate* cullData = m_GpuCull->BeginFrameUpload(frameIndex);

        // Skinned survivors are deferred to a second pass (they draw on the CPU-direct skinned
        // path after the static slots, which must stay contiguous from 0 for the GPU cull arrays).
        vector<u32> skinnedScratch;

        // Translucent survivors are routed to the forward translucent plan, sorted back-to-front
        // after the opaque/skinned slots are laid out (its DrawData slots follow theirs).
        vector<u32> translucentScratch;

        // Fill one slot per survivor whose submesh has a loaded material (a materialless or
        // not-yet-resident submesh is skipped, matching the direct draw it replaces). The slot
        // index is the dense candidate id the instance attribute carries.
        for (const u32 id : m_CullScratch)
        {
            const SubMeshCandidate& candidate = candidates[id];
            const VisibleMesh& item = view.Visible[candidate.MeshCandidate];
            const Mesh& mesh = *item.Mesh;
            const std::span<const AssetHandle<MaterialInstance>> materials = mesh.GetMaterials();
            const SubMesh& subMesh = mesh.GetSubMeshes()[candidate.SubMeshIndex];

            if (subMesh.MaterialIndex == SubMesh::NoMaterial ||
                !materials[subMesh.MaterialIndex].IsLoaded())
            {
                continue;
            }

            // Translucent submeshes are excluded from the g-buffer/opaque draw list and collected
            // into the forward translucent plan below (they output final color through the forward
            // pass, not the g-buffer). The frustum-survivor set is shared — a translucent submesh is
            // simply routed to a different draw list.
            if (materials[subMesh.MaterialIndex].Get()->GetDomain() == MaterialDomain::Translucent)
            {
                translucentScratch.push_back(id);
                continue;
            }

            if (mesh.IsSkinned())
            {
                skinnedScratch.push_back(id);
                continue;
            }

            const u32 slot = static_cast<u32>(plan.Slots.size());
            if (slot >= MaxCullCandidates)
            {
                VE_ASSERT(false,
                          "SceneRenderer: per-frame candidate count exceeds MaxCullCandidates {}",
                          MaxCullCandidates);
                break;
            }

            const MaterialInstance& material = *materials[subMesh.MaterialIndex].Get();
            if (!plan.PipelineMaterial)
            {
                plan.PipelineMaterial = materials[subMesh.MaterialIndex].Get();
            }

            // Per-draw record: world matrix, the normal matrix's three columns (inverse-
            // transpose of the upper 3×3, correct under non-uniform scale), and the
            // frame-folded material selector.
            const mat3 normalMatrix = glm::inverseTranspose(mat3(item.World));
            // Previous world for velocity: last frame's matrix for this entity, or the
            // current one (zero object motion) when first seen.
            mat4 prevWorld = item.World;
            {
                const auto it = m_PreviousWorlds.find(PackEntity(item.Owner));
                if (it != m_PreviousWorlds.end())
                {
                    prevWorld = it->second;
                }
            }
            drawData[frameBase + slot] = GpuDrawData{
                .World = item.World,
                .NormalColumn0 = vec4(normalMatrix[0], 0.0f),
                .NormalColumn1 = vec4(normalMatrix[1], 0.0f),
                .NormalColumn2 = vec4(normalMatrix[2], 0.0f),
                .MaterialIndex = material.GetMaterialSelector(),
                .EntityIndex = item.Owner.Index,
                .PrevWorld = prevWorld,
            };

            if (cullData != nullptr)
            {
                const AABB& bounds = item.WorldBounds;
                cullData[slot] = GpuCullCandidate{
                    .BoundsMin = vec4(bounds.Min, 0.0f),
                    .BoundsMax = vec4(bounds.Max, 0.0f),
                    .IndexCount = subMesh.IndexCount,
                    .FirstIndex = subMesh.IndexOffset,
                    .VertexOffset = 0,
                    .FirstInstance = slot,
                };
            }

            plan.Slots.push_back(DrawSlot{
                .SourceMesh = &mesh,
                .Pipeline = materials[subMesh.MaterialIndex].Get(),
                .IndexCount = subMesh.IndexCount,
                .FirstIndex = subMesh.IndexOffset,
                .VertexOffset = 0,
                .CandidateId = slot,
            });
        }

        // Group contiguous slots that share both a source mesh and a pipeline, so the mesh's
        // buffers and the material pipeline each bind once per group. Splitting on the pipeline
        // (not just the mesh) is what lets surface materials with different fragment shaders
        // coexist — each group binds its own.
        for (u32 s = 0; s < plan.Slots.size();)
        {
            const Mesh* mesh = plan.Slots[s].SourceMesh;
            const MaterialInstance* pipeline = plan.Slots[s].Pipeline;
            u32 count = 0;
            while (s + count < plan.Slots.size() && plan.Slots[s + count].SourceMesh == mesh &&
                   plan.Slots[s + count].Pipeline == pipeline)
            {
                ++count;
            }
            plan.Groups.push_back(DrawGroup{.SourceMesh = mesh,
                                            .PipelineMaterial = pipeline,
                                            .FirstSlot = s,
                                            .SlotCount = count});
            s += count;
        }

        // Skinned second pass: assign DrawData slots after the static ones (keeping the static
        // range contiguous from 0), compute each skinned instance's palette once per entity, and
        // record PaletteBase into the slot's DrawData. These draw on the CPU-direct skinned path.
        for (const u32 id : skinnedScratch)
        {
            const SubMeshCandidate& candidate = candidates[id];
            const VisibleMesh& item = view.Visible[candidate.MeshCandidate];
            const Mesh& mesh = *item.Mesh;
            const SubMesh& subMesh = mesh.GetSubMeshes()[candidate.SubMeshIndex];
            const std::span<const AssetHandle<MaterialInstance>> materials = mesh.GetMaterials();
            const AssetHandle<Skeleton>& skeletonHandle = mesh.GetSkeleton();
            if (!skeletonHandle.IsLoaded())
            {
                continue;
            }
            const Skeleton& skeleton = *skeletonHandle.Get();
            const u32 boneCount = static_cast<u32>(skeleton.GetBoneCount());

            // One palette per entity, shared by its submeshes. Computed on first encounter from
            // the entity's SkinnedPose (the animation system's output) or the bind pose when the
            // entity has none (e.g. the editor with systems paused).
            const u64 packed = PackEntity(item.Owner);
            u32 paletteBase = 0;
            const auto existing = m_PaletteBaseByEntity.find(packed);
            if (existing != m_PaletteBaseByEntity.end())
            {
                paletteBase = existing->second;
            }
            else
            {
                if (paletteCursor + boneCount > MaxSkinningMatricesPerFrame)
                {
                    continue; // palette budget exhausted this frame
                }
                paletteBase = paletteRegionBase + paletteCursor;

                const auto* pose = view.World.TryGet<SkinnedPose>(item.Owner);
                if (pose != nullptr && pose->Skinning.size() == boneCount)
                {
                    std::memcpy(paletteData + paletteBase, pose->Skinning.data(),
                                static_cast<usize>(boneCount) * sizeof(mat4));
                }
                else
                {
                    vector<mat4> bind;
                    skeleton.ComputeBindPoseMatrices(bind);
                    std::memcpy(paletteData + paletteBase, bind.data(),
                                static_cast<usize>(boneCount) * sizeof(mat4));
                }

                paletteCursor += boneCount;
                m_PaletteBaseByEntity[packed] = paletteBase;
            }

            const u32 slot = static_cast<u32>(plan.Slots.size() + plan.SkinnedSlots.size());
            if (slot >= MaxCullCandidates)
            {
                break;
            }

            const MaterialInstance& material = *materials[subMesh.MaterialIndex].Get();
            if (plan.SkinnedPipelineMaterial == nullptr)
            {
                plan.SkinnedPipelineMaterial = materials[subMesh.MaterialIndex].Get();
            }

            // Velocity needs the previous frame's world and palette base for this entity (its
            // deformation motion). The previous palette data is still resident in its own ring
            // region. First seen → no motion (current values).
            mat4 prevWorld = item.World;
            u32 prevPaletteBase = paletteBase;
            {
                const auto prevWorldIt = m_PreviousWorlds.find(packed);
                if (prevWorldIt != m_PreviousWorlds.end())
                {
                    prevWorld = prevWorldIt->second;
                }
                const auto prevBaseIt = m_PreviousPaletteBaseByEntity.find(packed);
                if (prevBaseIt != m_PreviousPaletteBaseByEntity.end())
                {
                    prevPaletteBase = prevBaseIt->second;
                }
            }

            const mat3 normalMatrix = glm::inverseTranspose(mat3(item.World));
            drawData[frameBase + slot] = GpuDrawData{
                .World = item.World,
                .NormalColumn0 = vec4(normalMatrix[0], 0.0f),
                .NormalColumn1 = vec4(normalMatrix[1], 0.0f),
                .NormalColumn2 = vec4(normalMatrix[2], 0.0f),
                .MaterialIndex = material.GetMaterialSelector(),
                .PaletteBase = paletteBase,
                .PrevPaletteBase = prevPaletteBase,
                .EntityIndex = item.Owner.Index,
                .PrevWorld = prevWorld,
            };

            plan.SkinnedSlots.push_back(DrawSlot{
                .SourceMesh = &mesh,
                .Pipeline = materials[subMesh.MaterialIndex].Get(),
                .IndexCount = subMesh.IndexCount,
                .FirstIndex = subMesh.IndexOffset,
                .VertexOffset = 0,
                .CandidateId = slot,
            });
        }

        for (u32 s = 0; s < plan.SkinnedSlots.size();)
        {
            const Mesh* mesh = plan.SkinnedSlots[s].SourceMesh;
            const MaterialInstance* pipeline = plan.SkinnedSlots[s].Pipeline;
            u32 count = 0;
            while (s + count < plan.SkinnedSlots.size() &&
                   plan.SkinnedSlots[s + count].SourceMesh == mesh &&
                   plan.SkinnedSlots[s + count].Pipeline == pipeline)
            {
                ++count;
            }
            plan.SkinnedGroups.push_back(DrawGroup{.SourceMesh = mesh,
                                                   .PipelineMaterial = pipeline,
                                                   .FirstSlot = s,
                                                   .SlotCount = count});
            s += count;
        }

        // Forward translucent gather: one DrawData slot per translucent survivor, allocated after
        // the opaque static + skinned slots so those stay contiguous from 0 for the GPU cull
        // arrays. Each draw reads its record from DrawData by the candidate id, exactly like a
        // static surface draw; the translucent pass binds each material's own alpha-blended
        // pipeline. The forward pass draws through the canonical (static) vertex layout, so a
        // skinned mesh carrying a translucent material is not gathered here (opaque skinning,
        // which uses the skinned vertex path, is unaffected).
        const mat4 viewMatrix = view.Camera.View();
        for (const u32 id : translucentScratch)
        {
            const SubMeshCandidate& candidate = candidates[id];
            const VisibleMesh& item = view.Visible[candidate.MeshCandidate];
            const Mesh& mesh = *item.Mesh;
            if (mesh.IsSkinned())
            {
                continue;
            }
            const std::span<const AssetHandle<MaterialInstance>> materials = mesh.GetMaterials();
            const SubMesh& subMesh = mesh.GetSubMeshes()[candidate.SubMeshIndex];

            const u32 slot = static_cast<u32>(plan.Slots.size() + plan.SkinnedSlots.size() +
                                              translucentPlan.Draws.size());
            if (slot >= MaxCullCandidates)
            {
                break;
            }

            const MaterialInstance& material = *materials[subMesh.MaterialIndex].Get();

            const mat3 normalMatrix = glm::inverseTranspose(mat3(item.World));
            mat4 prevWorld = item.World;
            {
                const auto it = m_PreviousWorlds.find(PackEntity(item.Owner));
                if (it != m_PreviousWorlds.end())
                {
                    prevWorld = it->second;
                }
            }
            drawData[frameBase + slot] = GpuDrawData{
                .World = item.World,
                .NormalColumn0 = vec4(normalMatrix[0], 0.0f),
                .NormalColumn1 = vec4(normalMatrix[1], 0.0f),
                .NormalColumn2 = vec4(normalMatrix[2], 0.0f),
                .MaterialIndex = material.GetMaterialSelector(),
                .EntityIndex = item.Owner.Index,
                .PrevWorld = prevWorld,
            };

            // Sort key: the submesh center in view space. The camera looks down -Z, so a farther
            // submesh has a more negative z; sorting ascending by z draws farthest first.
            const vec3 center = (item.WorldBounds.Min + item.WorldBounds.Max) * 0.5f;
            const f32 viewDepth = (viewMatrix * vec4(center, 1.0f)).z;

            translucentPlan.Draws.push_back(TranslucentDraw{
                .Material = &material,
                .SourceMesh = &mesh,
                .IndexCount = subMesh.IndexCount,
                .FirstIndex = subMesh.IndexOffset,
                .CandidateId = slot,
                .ViewDepth = viewDepth,
                .SortPriority = material.GetParent().Get()->GetSortPriority(),
            });
        }

        // Ascending priority groups, back-to-front (most negative view-space z first) within
        // each: a higher-priority material (an overlay) draws over every lower-priority draw
        // regardless of depth.
        std::ranges::sort(translucentPlan.Draws,
                          [](const TranslucentDraw& a, const TranslucentDraw& b)
                          {
                              if (a.SortPriority != b.SortPriority)
                              {
                                  return a.SortPriority < b.SortPriority;
                              }
                              return a.ViewDepth < b.ViewDepth;
                          });

        // Arm this frame's GPU cull dispatch + survivor readback: the count is the opaque static
        // slot count, the previous-frame view-projection screen-bounds candidates, and Occlusion
        // gates the occlusion test (a history-invalid frame stays frustum-only inside the subsystem).
        if (m_GpuCull->GetActiveCull() == SceneRendererSettings::CullMode::GPU)
        {
            m_GpuCull->RecordFrame(static_cast<u32>(plan.Slots.size()), frameBase, frameIndex,
                                   m_PreviousViewProj, m_Settings.Occlusion);
        }
    }

    void SceneRenderer::Resize(const uvec2 extent)
    {
        m_Extent = extent;
        // The new allocation is the full target until the next Execute scales it; the prior
        // sub-rect mapping is moot (the TAA history resets on resize anyway).
        m_ValidExtent = extent;
        m_PreviousRenderScaleUV = vec2(1.0f);
        m_PreviousMaxValidUV = vec2(1.0f);
        CreateOutput();
        CreateGBuffer();
        CreateHdr();
        m_Taa->Resize(m_Extent, m_Settings.TAA);
        m_Bloom->Resize(m_Extent, m_HdrView);
        m_Ssr->Recreate(m_Settings, m_Extent, m_DepthView, m_GpuCull->GetHiZReduceSetLayout(),
                        m_Bloom->GetDownUpSetLayout());
        m_Dof->Recreate(m_Settings, m_Extent, m_HdrView, m_DepthView,
                        m_Bloom->GetDownUpSetLayout());
        m_Refraction->Recreate(m_Settings, m_Extent);
        // The HDR target moved; rebind the metering source and re-snap the adaptation so the
        // resized frame is not mis-exposed off a stale ring value.
        m_AutoExposure->RebindHdr(m_HdrView);
        m_AutoExposure->RequestReset();
        m_Shadows->Reconfigure(m_Settings);
        m_Picking->Recreate(m_Settings, m_Extent);
        Rebuild();
    }

    u32 SceneRenderer::GetMaxShadowResolution() const
    {
        return ShadowSystem::GetMaxShadowResolution(m_Context);
    }

    u32 SceneRenderer::GetMaxPunctualShadowResolution() const
    {
        return ShadowSystem::GetMaxPunctualShadowResolution(m_Context);
    }

    void SceneRenderer::Configure(const SceneRendererSettings& settings)
    {
        m_Settings = settings;
        ShadowSystem::ClampResolutions(m_Context, m_Settings);
        m_GpuCull->ResolveActiveCullMode(m_Settings);
        m_Taa->Resize(m_Extent, m_Settings.TAA);
        // The bloom pyramid is extent-driven (unchanged here); only the kernel choice may change.
        m_Bloom->Reconfigure(m_Settings.Kernel);
        m_Ssr->Recreate(m_Settings, m_Extent, m_DepthView, m_GpuCull->GetHiZReduceSetLayout(),
                        m_Bloom->GetDownUpSetLayout());
        m_Dof->Recreate(m_Settings, m_Extent, m_HdrView, m_DepthView,
                        m_Bloom->GetDownUpSetLayout());
        m_Refraction->Recreate(m_Settings, m_Extent);
        m_Shadows->Reconfigure(m_Settings);
        m_Picking->Recreate(m_Settings, m_Extent);
        Rebuild();
    }

    void SceneRenderer::Execute(CommandBuffer& cmd, const SceneView& view)
    {
        // The DebugDrawScenePass consumes this frame's accumulator below; clear it after so the
        // next frame starts empty (immediate-mode: every primitive is re-pushed each frame).
        struct DebugDrawClearGuard
        {
            DebugDraw& Accumulator;
            ~DebugDrawClearGuard() { Accumulator.Clear(); }
        } const debugDrawClearGuard{m_DebugDraw};

        // Dynamic resolution: render into the top-left round(allocExtent * scale) sub-rect of the
        // (high-water-mark-allocated) targets; the terminal tonemap upscales it to the full output.
        const FrameScale scale = ResolveRenderScale(view);
        const uvec2 validExtent = scale.ValidExtent;
        const vec2 renderScaleUV = scale.RenderScaleUV;
        const vec2 maxValidUV = scale.MaxValidUV;

        // Auto-exposure: the meter reads the histogram a completed frame wrote, eases the adapted
        // luminance, and resolves the exposure the tonemap uses (SceneView::Exposure directly when
        // metering is inactive). The bloom bright-pass later reads the same resolved exposure.
        const f32 exposure = m_AutoExposure->ResolveExposure(view, m_AutoExposureActive);

        // Per-frame param writes land in the ring-buffered block's current region (no stall).
        if (m_TonemapMaterial.IsLoaded())
        {
            MaterialInstance& tonemap = *m_TonemapMaterial.Get();
            tonemap.SetParam("Exposure", exposure);
            // The tone curve selector, carried as a float the fragment casts back to the enum.
            tonemap.SetParam("Tonemapper", static_cast<f32>(static_cast<u32>(view.Tonemapper)));
            // The terminal tonemap reads the sub-rect HDR and upscales it to the full output.
            tonemap.SetParam("RenderScale", vec4(renderScaleUV, maxValidUV));
        }

        // Sync the broadphase first: re-gathers and rebuilds only when the scene's spatial
        // version moved or a mesh finished loading. The scene passes then query its tree.
        // sceneBounds is the bound union from the same gather, so no separate SceneBounds call is
        // needed; casterBounds excludes the non-casters (so a light's own co-located body never
        // widens its shadow frustum) and drives both the cascade near-extension and the punctual
        // spot/area fit.
        m_Broadphase.Sync(view.World);
        const AABB sceneBounds = m_Broadphase.GetSceneBounds();
        const AABB casterBounds = m_Broadphase.GetCasterBounds();

        // Pack every Light entity into the GPU light layout: directional selection,
        // punctual shadow-slot assignment, and the std430 per-light records. The first
        // MaxShadowedPunctual point/spot lights are assigned a shadow slot (Cone.z carries
        // it, -1 = unshadowed); their records ride set-1 binding 3. The caster bound fits each
        // spot/area light's shadow frustum to the geometry it must shadow.
        const PackedSceneLights packed =
            PackSceneLights(view.World, m_Settings.PunctualShadows,
                            m_Settings.PunctualShadowResolution, casterBounds);

        // Mirror filled records into the GPU block (unused slots stay zeroed → type 0 = "no map").
        // AtlasParams.x = 1/tileRes, used by the lighting pass for inset clamping and PCF.
        PunctualShadowBlock punctualBlock{};
        for (u32 s = 0; s < packed.PunctualCount; ++s)
        {
            punctualBlock.Records[s] = packed.PunctualRecords[s];
        }
        punctualBlock.AtlasParams =
            vec4(1.0f / static_cast<f32>(m_Settings.PunctualShadowResolution), 0.0f, 0.0f, 0.0f);

        // sceneBounds extends only the per-cascade light-axis cull near plane (off-screen
        // casters); the cascade XY extent comes from the camera frustum slice. With depth
        // clamp the render near stays tight and the shadow pipelines pancake nearer casters
        // onto it — PancakeNear must match the pipelines' DepthClampEnable.
        const CascadeData cascades =
            ComputeCascades(view.Camera, packed.DirectionalTravel, casterBounds,
                            {.Count = m_Settings.CascadeCount,
                             .Lambda = m_Settings.CascadeSplitLambda,
                             .Resolution = m_Settings.ShadowResolution,
                             .MaxDistance = m_Settings.MaxShadowDistance,
                             .PancakeNear = m_Context.IsDepthClampSupported()});

        // Thread the raw (non-tile-remapped) cascade matrices to the shadow pass,
        // which renders each cascade with its viewport placing it in the atlas tile.
        SceneView resolvedView = view;
        resolvedView.RenderExtent = validExtent;
        resolvedView.LightCount = packed.LightCount;
        resolvedView.CascadeViewProj = cascades.ViewProj;
        resolvedView.CascadeCullViewProj = cascades.CullViewProj;
        resolvedView.CascadeCount = cascades.Count;
        resolvedView.PunctualShadows = packed.PunctualRecords;
        resolvedView.PunctualShadowCount = packed.PunctualCount;
        resolvedView.PunctualShadowRawViewProj = packed.PunctualRawViewProj;
        resolvedView.Visible = m_Broadphase.GetCandidates();
        resolvedView.Broadphase = &m_Broadphase;

        // Fixed-timestep render interpolation: blend each candidate's world transform between the
        // scene's last two Sim-tick snapshots by the frame's alpha, so a 60 Hz sim renders smoothly
        // at a higher frame rate.
        ApplyTransformInterpolation(view, resolvedView);

        // Resolve the scene's sky / point-field / volume-field components into this Execute — the
        // lights model, recompiling the pass set at this frame boundary on a sky source/tier change
        // or a field appearing/disappearing (reusing the imported output so GetOutput() stays valid).
        ResolveScenePasses(resolvedView);

        BindlessRegistry& registry = m_Context.GetBindlessRegistry();

        // The sky-resolve subsystem records its pre-graph generation before the frame's BeginView:
        // the atmosphere LUT gate (a baked atmosphere on its own immediate-submit path), the
        // baked-sky cube bake on the dirty signal, the SH-tier readback projection, the IBL-tier
        // convolution, and the environment-sky SH projection. The bake writes six face
        // view-constants regions into distinct view slots, so it must run ahead of the frame's own
        // BeginView below.
        m_SkyResolver->RecordPreBeginView(cmd, resolvedView, m_SkyPipeline);

        // Claim this Execute's view slot before any shared-buffer write below: the view-constants
        // and light buffers are shared across every viewport, so each render writes its own region
        // rather than clobbering the one another viewport's draws still read this frame.
        registry.BeginView();
        registry.WriteLights(std::as_bytes(std::span(packed.Lights.data(), packed.LightCount)));
        registry.WriteAreaVertices(
            std::as_bytes(std::span(packed.AreaVertices.data(), packed.AreaVertexCount)));

        // Pack view constants (camera/view state only; shadow system rides set-1).
        // The unjittered view-projection drives the frustum cull, hi-Z, and next frame's
        // reprojection matrix; the jittered one (TAA only) is what the geometry and
        // lighting actually render through.
        const mat4 viewProj = view.Camera.ViewProjection();
        mat4 renderProj = view.Camera.Projection();
        if (m_TaaActive && validExtent.x > 0 && validExtent.y > 0)
        {
            // Sub-pixel projection shear; sign is irrelevant to quality (the sequence is
            // symmetric) and cancels between render and reconstruction, which share this
            // matrix. The reprojection uses the separate unjittered PrevViewProj. The jitter is
            // a fraction of the rendered (sub-rect) extent, the resolution actually rasterized.
            const vec2 jitterPixel = TaaJitterOffset(m_FrameIndex);
            renderProj[2][0] += 2.0f * jitterPixel.x / static_cast<f32>(validExtent.x);
            renderProj[2][1] += 2.0f * jitterPixel.y / static_cast<f32>(validExtent.y);
        }
        // SH skylight: the lighting pass's second ambient arm reads the sky SH from the
        // view-constants block below. Every SH-tier source projects the one radiance cube it fills —
        // an environment its equirect cube, a baked material or baked atmosphere its bake cube — on
        // that source's dirty signal above; the projection is a pure cube→SH read, so display and
        // ambient agree.
        const bool skylightActive = m_SkyResolver->IsSkylightActive();
        const Sh9& skySh = m_SkyResolver->GetSkySh();

        const mat4 renderViewProj = renderProj * view.Camera.View();
        ViewConstantsBlock viewConstants{
            .InvViewProj = glm::inverse(renderViewProj),
            .CameraPosition = vec4(view.Camera.GetPosition(), 0.0f),
            .View = view.Camera.View(),
            .Proj = renderProj,
            .PrevViewProj = m_PreviousViewProj,
            .CurViewProj = viewProj,
            .RenderScaleUV = vec4(renderScaleUV, m_PreviousRenderScaleUV),
            .MaxValidUV = vec4(maxValidUV, m_PreviousMaxValidUV),
            // The frame clock is engine-global (Time), frame-locked so every view and material
            // reads one consistent value; the delta is this view's.
            .TimeParams = vec4(Time::GetFrameTime(), view.Delta, 0.0f, 0.0f),
            .ExtentParams = vec4(vec2(validExtent), vec2(m_Extent)),
            .SceneColor = uvec4(m_Refraction->GetSceneHandle().Index, m_SamplerHandle.Index,
                                m_RefractionActive ? 1u : 0u, m_Refraction->GetDepthHandle().Index),
        };
        for (u32 i = 0; i < ShCoefficientCount; ++i)
        {
            viewConstants.SkyShCoeffs[i] =
                skylightActive ? vec4(skySh.Coefficients[i], 0.0f) : vec4(0.0f);
        }
        registry.WriteViewConstants(std::as_bytes(std::span(&viewConstants, 1)));

        // Decide whether last frame's pyramid is trustworthy this frame; the GPU cull subsystem
        // combines the reset gate (frame 0 / post-resize / post-configure) with the device-free
        // view-delta metric. The result feeds the GPU cull (occlusion skipped when invalid).
        const mat4 invView = glm::inverse(view.Camera.View());
        const HiZHistoryState currentHiZState{
            .CameraPosition = view.Camera.GetPosition(),
            // The camera looks down -Z in view space; its world forward is the negated
            // third basis column of the view's inverse.
            .CameraForward = glm::normalize(-vec3(invView[2])),
            .Projection = view.Camera.Projection(),
        };
        const f32 sceneDiagonal = sceneBounds.IsEmpty() ? 0.0f : glm::length(sceneBounds.Size());
        m_GpuCull->EvaluateHistory(currentHiZState, sceneDiagonal);

        // Pack set-1 ShadowConstants: tile-remapped cascade view-projs, splits, and
        // params. Enabled only when the shadow pass is wired AND a directional light
        // exists this frame; otherwise the lighting pass reads full visibility.
        const bool shadowEnabled = m_ShadowActive && m_ShadowPass && packed.HaveDirectional;
        const ShadowConstantsBlock shadowConstants =
            PackShadowConstants(m_Settings, cascades, shadowEnabled);

        // Write this frame's shadow + punctual ring regions (not yet submitted; safe).
        const u32 frameIndex = m_Context.GetCurrentFrameInFlight();
        m_Shadows->WriteFrameConstants(frameIndex, shadowConstants, punctualBlock);

        // Read the GPU survivor count the previous Execute wrote into this frame's region
        // before PrepareDraws zeroes it again — the host-visible count is one frame late, so
        // it never gates this frame's draw. Only meaningful under the GPU path.
        if (m_GpuCull->GetActiveCull() == SceneRendererSettings::CullMode::GPU)
        {
            m_GpuCull->ReadSurvivorCount(frameIndex);
        }

        // Fill the per-draw DrawData buffer + (GPU) the candidate buffer and submission plan.
        // The surface push's ViewConstantsIndex is the shared per-view slot, distinct from the
        // renderer-owned frame-in-flight rings PrepareDraws indexes internally.
        PrepareDraws(resolvedView, registry.GetCurrentViewConstantsIndex());

        // Build the id-writing pipeline variants on the first frame a surface material is available
        // (their layout is shared across surface materials), so the picking pass can re-draw the
        // same survivors. A no-op when picking is off or the pipelines are already built.
        m_Picking->EnsurePipelines(m_Internal->Plan.PipelineMaterial,
                                   m_Internal->Plan.SkinnedPipelineMaterial);

        // Expose the skinning palette + per-entity bases (filled by PrepareDraws) to the shadow
        // passes so a skinned caster casts its posed shadow.
        resolvedView.SkinningPalette = m_PaletteSet;
        resolvedView.SkinnedPaletteBases = &m_PaletteBaseByEntity;

        // Assemble this frame's graph import bindings — the always-bound targets plus the
        // conditionally-declared battery imports, matched to the compiled graph's declared imports.
        const vector<RenderGraph::ImportBinding> bindings = BuildImportBindings();

        // Image-based lighting: the sky-resolve subsystem initializes the BRDF LUT + leaves the maps
        // in a sampled layout once, then (re)generates the radiance/irradiance/prefilter maps when
        // the bound environment changes — recorded into cmd after the import bindings are built and
        // before the graph the lighting pass samples them through.
        m_SkyResolver->RecordPreReplay(cmd, resolvedView);

        // The atmosphere LUTs were generated ahead of the atmosphere bake, before the frame's
        // BeginView (a baked atmosphere reads them per face); nothing more to record here.

        m_Internal->Graph->Execute(cmd, bindings, &resolvedView);

        // Service a pending pick: the picking subsystem transitions the EntityId target to
        // TransferSrc and copies the search window under the cursor into its readback buffer on the
        // graphics queue; the result becomes readable through PollPickId once this frame completes.
        m_Picking->ServiceRequest(cmd, m_Extent, m_FrameIndex);

        // Commit this frame's hi-Z history state: the reduction declared in this graph wrote the
        // pyramid from this frame's depth, so it pairs with this frame's view-projection next time.
        m_GpuCull->CommitHistory(currentHiZState);

        // Record this frame's view-projection, sub-rect mapping, and per-entity history for the
        // next frame's reprojection and velocity channel.
        RecordFrameHistory(viewProj, scale);
    }

    SceneRenderer::FrameScale SceneRenderer::ResolveRenderScale(const SceneView& view) const
    {
        // Scale applies only on the Final path with the sub-rect-aware battery set: a debug view, the
        // TAA resolve, the GPU hi-Z occlusion test, and the Dual-Kawase bloom kernel do not carry the
        // sub-rect sampling yet, so each forces full resolution (correct, just no scaling).
        const bool drsSupported =
            m_Settings.Mode == DebugView::Final && !m_Settings.TAA && !m_Settings.SSR &&
            !(m_GpuCull->GetActiveCull() == SceneRendererSettings::CullMode::GPU &&
              m_Settings.Occlusion) &&
            !(m_Settings.Bloom && m_Settings.Kernel == BloomKernel::Kawase);
        const f32 renderScale = drsSupported ? view.RenderScale : 1.0f;
        const uvec2 validExtent =
            glm::clamp(uvec2(glm::round(vec2(m_Extent) * renderScale)), uvec2(1), m_Extent);
        // Half-texel inset so a bilinear tap at the valid edge never reads past it.
        return {
            .ValidExtent = validExtent,
            .RenderScaleUV = vec2(validExtent) / vec2(m_Extent),
            .MaxValidUV = (vec2(validExtent) - 0.5f) / vec2(m_Extent),
        };
    }

    void SceneRenderer::ApplyTransformInterpolation(const SceneView& view, SceneView& resolvedView)
    {
        // The broadphase tree stays built from the current-tick transforms (its cull is
        // conservative, so a sub-tick offset never drops a visible submesh); only the drawn worlds
        // interpolate. A static scene reports no motion history and skips the copy, so its draw is
        // byte-identical to the un-interpolated path.
        if (view.Alpha == 0.0f || !view.World.HasTransformInterpolation())
        {
            return;
        }

        const std::span<const VisibleMesh> candidates = m_Broadphase.GetCandidates();
        m_InterpolatedCandidates.assign(candidates.begin(), candidates.end());
        for (VisibleMesh& candidate : m_InterpolatedCandidates)
        {
            candidate.World = view.World.GetInterpolatedWorldTransform(candidate.Owner, view.Alpha);
            candidate.WorldBounds = candidate.Mesh->GetBounds().Transformed(candidate.World);
        }
        resolvedView.Visible = m_InterpolatedCandidates;
    }

    void SceneRenderer::ResolveScenePasses(SceneView& resolvedView)
    {
        // Resolve the scene's one Sky component into this frame's sky fields — the lights model,
        // the renderer reading the component off the scene the way it reads the lights. A resolved
        // source-kind or lighting-tier change recompiles the pass set at this frame boundary,
        // reusing the imported output so GetOutput() stays valid (only Resize/Configure recreate it).
        m_SkyResolver->Resolve(resolvedView);
        if (m_SkyResolver->NeedsRecompile())
        {
            Rebuild();
        }

        // Resolve the scene's point-field components into this Execute's live field set the same
        // way — the pass inserts on the first live field and drops when the last one goes.
        ResolvePointFields(resolvedView);

        // Resolve the scene's volume-field components the same way — the volume march pass inserts on
        // the first live field and drops when the last one goes.
        ResolveVolumeFields(resolvedView);

        // Forward the resolved authored sky material to the sky-material pass (a no-op when the pass
        // is absent or no material is bound). The game has already written the material's own
        // params/handles (e.g. SetStorageBufferHandle) before Render.
        if (m_SkyMaterialPass != nullptr)
        {
            m_SkyMaterialPass->SetMaterial(resolvedView.SkyMaterial);
        }
    }

    vector<RenderGraph::ImportBinding> SceneRenderer::BuildImportBindings()
    {
        // Bloom and SSAO imports are appended only when active (they are only declared then).
        vector<RenderGraph::ImportBinding> bindings = {
            {m_AlbedoId, m_AlbedoView}, {m_NormalId, m_NormalView}, {m_OrmId, m_OrmView},
            {m_DepthId, m_DepthView},   {m_HdrId, m_HdrView},       {m_OutputId, m_OutputView},
        };
        if (m_TaaActive)
        {
            bindings.push_back({m_LitId, m_Taa->GetLitView()});
            bindings.push_back({m_TaaHistoryId, m_Taa->GetHistoryView()});
        }
        // Velocity is a g-buffer channel the surface pass writes every frame, so it is always bound.
        bindings.push_back({m_VelocityId, m_VelocityView});
        // Emissive (G4) is likewise a g-buffer channel written every frame, always bound.
        bindings.push_back({m_EmissiveId, m_EmissiveView});
        m_Picking->AppendBindings(bindings);
        if (m_ShadowActive && m_ShadowPass)
        {
            bindings.push_back({m_ShadowId, m_ShadowPass->GetShadowView()});
        }
        if (m_PunctualShadowActive && m_PunctualShadowPass)
        {
            bindings.push_back({m_PunctualShadowId, m_Shadows->GetPunctualView()});
        }
        if (m_BloomActive)
        {
            // Each pyramid mip binds its per-frame storage view to its per-mip import slot
            // (the down/up sweep declared per-level access on these); the result is one slot.
            const std::vector<Ref<ImageView>>& bloomMips = m_Bloom->GetMipViews();
            for (u32 level = 0; level < bloomMips.size(); level++)
            {
                bindings.push_back({m_BloomChainId.Level(level), bloomMips[level]});
            }
            bindings.push_back({m_BloomResultId, m_Bloom->GetResultView()});
        }
        if (m_AutoExposureActive)
        {
            bindings.push_back(
                {.Id = m_AutoExposureId, .Buffer = m_AutoExposure->GetHistogramBuffer()});
        }
        if (m_SsaoActive && m_SsaoPass != nullptr)
        {
            bindings.push_back({m_SsaoId, m_SsaoPass->GetAoView()});
        }
        if (m_RefractionActive)
        {
            bindings.push_back({m_RefractionSceneId, m_Refraction->GetSceneView()});
            bindings.push_back({m_RefractionDepthId, m_Refraction->GetDepthView()});
        }
        if (m_SsrActive)
        {
            bindings.push_back({m_SsrSceneId, m_Ssr->GetSceneView()});
            // Each reflection mip binds its per-frame view to its per-mip import slot (the trace
            // writes mip 0, the blur the rest).
            const std::vector<Ref<ImageView>>& reflectionMips = m_Ssr->GetReflectionMipViews();
            for (u32 level = 0; level < reflectionMips.size(); level++)
            {
                bindings.push_back({m_SsrReflectionChainId.Level(level), reflectionMips[level]});
            }
            const std::vector<Ref<ImageView>>& hiZMips = m_Ssr->GetHiZMipViews();
            for (u32 level = 0; level < hiZMips.size(); level++)
            {
                bindings.push_back({m_SsrHiZChainId.Level(level), hiZMips[level]});
            }
        }
        if (m_DofActive)
        {
            bindings.push_back({m_DofNearId, m_Dof->GetNearView()});
            bindings.push_back({m_DofFarId, m_Dof->GetFarView()});
            bindings.push_back({m_DofCocId, m_Dof->GetCocView()});
            bindings.push_back({m_DofTileId, m_Dof->GetTileView()});
        }
        if (m_DofComposited)
        {
            bindings.push_back({m_DofSceneId, m_Dof->GetSceneView()});
            bindings.push_back({m_DofNearBlurId, m_Dof->GetNearBlurView()});
            bindings.push_back({m_DofFarBlurId, m_Dof->GetFarBlurView()});
            bindings.push_back({m_DofNearFillId, m_Dof->GetNearFillView()});
            bindings.push_back({m_DofFarFillId, m_Dof->GetFarFillView()});
        }
        // Bind each hi-Z mip's per-frame storage view to its per-mip import slot.
        const std::vector<Ref<ImageView>>& hiZMipViews = m_GpuCull->GetHiZMipViews();
        for (u32 level = 0; level < hiZMipViews.size(); level++)
        {
            bindings.push_back({m_GpuCull->GetHiZChainId().Level(level), hiZMipViews[level]});
        }
        // The GPU cull arm shares the indirect command buffer between the cull pass and the
        // geometry pass through this import (the same buffer the cull set binding 2 writes).
        if (m_GpuCull->GetActiveCull() == SceneRendererSettings::CullMode::GPU)
        {
            bindings.push_back(
                {.Id = m_GpuCull->GetIndirectId(), .Buffer = m_GpuCull->GetIndirectBuffer()});
        }
        return bindings;
    }

    void SceneRenderer::RecordFrameHistory(const mat4& viewProj, const FrameScale& scale)
    {
        // Capture this frame's camera view-projection for next frame's history comparison and
        // occlusion test (paired with this frame's reduced pyramid).
        m_PreviousViewProj = viewProj;

        // Record this frame's sub-rect mapping: GetValidExtent reports it, and the next frame's
        // TAA resolve reprojects the history written at this scale (the zw of RenderScaleUV).
        m_ValidExtent = scale.ValidExtent;
        m_PreviousRenderScaleUV = scale.RenderScaleUV;
        m_PreviousMaxValidUV = scale.MaxValidUV;

        // The history-copy pass populated the history this frame, so the next resolve may
        // reproject against it. Advance the jitter sequence regardless of the TAA toggle so
        // enabling it mid-run does not restart the phase.
        m_Taa->ClearHistoryReset();
        ++m_FrameIndex;

        // This frame's worlds + skinning-palette bases become next frame's "previous" for the
        // velocity channel. The palette buffer is ring-buffered, so the previous bases still point
        // at resident data next frame.
        m_PreviousWorlds.swap(m_CurrentWorlds);
        m_PreviousPaletteBaseByEntity = m_PaletteBaseByEntity;
    }

    Ref<ImageView> SceneRenderer::GetOutput() const
    {
        return m_OutputView;
    }

    uvec2 SceneRenderer::GetValidExtent() const
    {
        return m_ValidExtent;
    }

    u32 SceneRenderer::GetLastVisibleCount() const
    {
        return static_cast<u32>(m_Broadphase.GetSubMeshCandidates().size());
    }
    SceneRendererSettings::CullMode SceneRenderer::GetActiveCullMode() const
    {
        return m_GpuCull->GetActiveCull();
    }
    u32 SceneRenderer::GetLastGpuSurvivorCount() const
    {
        return m_GpuCull->GetLastGpuSurvivorCount();
    }
    vector<u32> SceneRenderer::ReadbackGpuSurvivorFlags() const
    {
        return m_GpuCull->ReadbackGpuSurvivorFlags();
    }
    u32 SceneRenderer::GetFrustumSurvivedCount() const
    {
        return m_FrustumSurvivedCount;
    }
    u32 SceneRenderer::GetLastDrawnCount() const
    {
        return m_LastDrawnCount;
    }
    PointFieldStats SceneRenderer::GetPointFieldStats() const
    {
        // Sum the tail and scene-color passes into one funnel; no active pass (no live field this
        // frame) reads back as all-zero, matching the passes' own no-field-drawn frames.
        PointFieldStats stats{};
        auto fold = [&stats](const PointFieldStats& s)
        {
            stats.Fields += s.Fields;
            stats.CellsTotal += s.CellsTotal;
            stats.CellsInFrustum += s.CellsInFrustum;
            stats.CellsMeasured += s.CellsMeasured;
            stats.ResolvedDraws += s.ResolvedDraws;
            stats.SpritePoints += s.SpritePoints;
            stats.CompactedPoints += s.CompactedPoints;
            stats.Splats += s.Splats;
            if (s.DrawSource != SpriteDrawSource::None)
            {
                stats.DrawSource = s.DrawSource;
            }
        };
        if (m_PointFieldPass != nullptr)
        {
            fold(static_cast<const PointFieldScenePass*>(m_PointFieldPass.get())->GetStats());
        }
        if (m_ScenePointFieldPass != nullptr)
        {
            fold(static_cast<const PointFieldScenePass*>(m_ScenePointFieldPass)->GetStats());
        }
        return stats;
    }

    void SceneRenderer::SetPointFieldForceDirect(const bool force)
    {
        // Persist the choice so a recompile that rebuilds the passes reapplies it, then apply to
        // the live passes if any exist.
        m_PointFieldForceDirect = force;
        if (m_PointFieldPass != nullptr)
        {
            static_cast<PointFieldScenePass*>(m_PointFieldPass.get())->SetForceDirect(force);
        }
        if (m_ScenePointFieldPass != nullptr)
        {
            static_cast<PointFieldScenePass*>(m_ScenePointFieldPass)->SetForceDirect(force);
        }
    }

    bool SceneRenderer::DidBroadphaseRebuildLastFrame() const
    {
        return m_Broadphase.DidRebuildLastSync();
    }
    bool SceneRenderer::DidRegenerateAtmosphereLastFrame() const
    {
        return m_SkyResolver->DidRegenerateAtmosphereLastFrame();
    }
    u32 SceneRenderer::GetBroadphaseNodeCount() const
    {
        return m_Broadphase.GetNodeCount();
    }
    Ref<ImageView> SceneRenderer::GetAlbedoView() const
    {
        return m_AlbedoView;
    }
    Ref<ImageView> SceneRenderer::GetNormalView() const
    {
        return m_NormalView;
    }
    Ref<ImageView> SceneRenderer::GetOrmView() const
    {
        return m_OrmView;
    }
    Ref<ImageView> SceneRenderer::GetDepthView() const
    {
        return m_DepthView;
    }
    Ref<ImageView> SceneRenderer::GetHiZView() const
    {
        return m_GpuCull->GetHiZSampleView();
    }
    Ref<ImageView> SceneRenderer::GetHiZMipView(const u32 level) const
    {
        const std::vector<Ref<ImageView>>& mips = m_GpuCull->GetHiZMipViews();
        VE_ASSERT(level < mips.size(), "SceneRenderer::GetHiZMipView: level {} out of range",
                  level);
        return mips[level];
    }
    u32 SceneRenderer::GetHiZMipCount() const
    {
        return static_cast<u32>(m_GpuCull->GetHiZMipViews().size());
    }
    bool SceneRenderer::IsHiZHistoryValid() const
    {
        return m_GpuCull->IsHiZHistoryValid();
    }
    mat4 SceneRenderer::GetPreviousViewProj() const
    {
        return m_PreviousViewProj;
    }
    Ref<ImageView> SceneRenderer::GetHdrView() const
    {
        return m_HdrView;
    }
    Ref<ImageView> SceneRenderer::GetBloomResultView() const
    {
        return m_Bloom->GetResultView();
    }
    Ref<ImageView> SceneRenderer::GetTaaHistoryView() const
    {
        return m_Taa->GetHistoryView();
    }
    Ref<ImageView> SceneRenderer::GetVelocityView() const
    {
        return m_VelocityView;
    }
    Ref<ImageView> SceneRenderer::GetPunctualShadowView() const
    {
        return m_Shadows->GetPunctualView();
    }
    DebugDraw& SceneRenderer::GetDebugDraw() const
    {
        return m_DebugDraw;
    }

    void SceneRenderer::RequestPick(const uvec2 texel)
    {
        m_Picking->RequestPick(texel);
    }

    bool SceneRenderer::IsPickInFlight() const
    {
        return m_Picking->IsPickInFlight();
    }

    optional<u32> SceneRenderer::PollPickId()
    {
        return m_Picking->PollPickId(m_FrameIndex);
    }
}
