#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/LtcLut.h>
#include <Veng/Asset/RawAsset.h>

#include "AutoExposureMeter.h"
#include "BloomPyramid.h"
#include "DebugDrawScenePass.h"
#include "DrawPlan.h"
#include "EnvironmentIbl.h"
#include "GpuBlocks.h"
#include "GpuCullSystem.h"
#include "PickingSystem.h"
#include "Passes/AtmospherePrecompute.h"
#include "Passes/DebugBlitScenePasses.h"
#include "Passes/DeferredLightingScenePass.h"
#include "Passes/GBufferScenePass.h"
#include "Passes/PickingScenePass.h"
#include "Passes/PointFieldScenePass.h"
#include "Passes/SkyScenePass.h"
#include "Passes/TaaScenePass.h"
#include "Passes/TranslucentScenePass.h"
#include "Passes/VolumeScenePass.h"
#include "ShadowScenePass.h"
#include "PunctualShadowScenePass.h"
#include "ShadowSystem.h"
#include "RefractionGrab.h"
#include "SkyboxScenePass.h"
#include "SkyCubemapBake.h"
#include "SkyResolver.h"
#include "SsaoScenePass.h"
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
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/GBuffer.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/HiZHistory.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/LightPacking.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/PunctualShadows.h>
#include <Veng/Renderer/Sampler.h>
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
#include <Veng/Asset/Shader.h>
#include <Veng/Asset/Skeleton.h>

#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

namespace Veng::Renderer
{
    namespace
    {
        // The engine core pack's fullscreen shaders (the AssetManager auto-mounts the core pack).
        constexpr AssetId FullscreenVertId{0xF46DD3C6F2AE0628ULL};
        constexpr AssetId DeferredLightingFragId{0x6569EBAC0810CC1FULL};
        constexpr AssetId DeferredLightingSsaoFragId{0x6EEF5D26BAF2849FULL};
        constexpr AssetId DeferredLightingCascadesFragId{0x834ED7C05F336E01ULL};
        constexpr AssetId SkyboxFragId{0xFCA568CC3463618FULL};
        constexpr AssetId AtmosphereSkyFragId{0x7DC6D927B2DF7858ULL};
        // The baked LTC lookup tables (matrix table then magnitude table, RGBA32F) for area lights.
        constexpr AssetId LtcLutId{0x27644C3AE58BB0D3ULL};

        constexpr AssetId SsaoFragId{0xCCBA63DB760A4E8EULL};
        constexpr AssetId TonemapInstanceId{0xB5AA7227E8A2DC11ULL};
        constexpr AssetId AlbedoBlitFragId{0xF90F709155D04BE7ULL};
        constexpr AssetId NormalBlitFragId{0x5A2CD7B270EAE5CDULL};
        constexpr AssetId DepthBlitFragId{0xE05F5F86E72F96D5ULL};
        constexpr AssetId OrmBlitFragId{0x7992B54A844CB1E1ULL};
        constexpr AssetId AoBlitFragId{0x97974B40192934E4ULL};
        constexpr AssetId MotionBlitFragId{0xCCD40C76935382FDULL};
        constexpr AssetId ShadowBlitFragId{0x0B61D5D42DAEF190ULL};

        // Linear float HDR format for the lighting target and the tail's scene-color intermediates.
        // G1 uses the same format as a sampled color target, establishing RGBA16F
        // color-attachment + sampled support on the platform.
        constexpr Format HdrFormat = Format::RGBA16Sfloat;
        constexpr ImageUsage HdrUsage = ImageUsage::ColorAttachment | ImageUsage::Sampled;

        // Single-channel unorm format for the SSAO target; the renderer builds the
        // SSAO pipeline against this format, and SsaoScenePass owns the image.
        constexpr Format SsaoFormat = Format::R8Unorm;

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

    // The fullscreen debug-blit pipelines for the non-Final DebugView arms, folded out of the
    // header. Each arm's terminal blit selects one of these; the layouts are held only to keep the
    // pipelines' descriptor/push-constant declarations alive. Built once at Create.
    struct SceneRenderer::DebugBlitPipelines
    {
        // Blits the albedo g-buffer channel; reused for the Bloom, Emissive, and Reflections
        // sources (the source is a push value, the pipeline the same fullscreen blit).
        Ref<GraphicsPipeline> Albedo;
        Ref<PipelineLayout> AlbedoLayout;
        // Blits the world-normal g-buffer channel.
        Ref<GraphicsPipeline> Normal;
        Ref<PipelineLayout> NormalLayout;
        // Blits the depth buffer as a linear grey scale.
        Ref<GraphicsPipeline> Depth;
        Ref<PipelineLayout> DepthLayout;
        // Blits a packed-ORM channel (Roughness/Metallic/Occlusion), the channel a push value.
        Ref<GraphicsPipeline> Orm;
        Ref<PipelineLayout> OrmLayout;
        // Blits the SSAO target.
        Ref<GraphicsPipeline> Ao;
        Ref<PipelineLayout> AoLayout;
        // Blits the per-object velocity target colorized as an optical-flow field.
        Ref<GraphicsPipeline> Motion;
        Ref<PipelineLayout> MotionLayout;
        // Blits the directional shadow atlas raw depth; reads through a dedicated set 1, not bindless.
        Ref<GraphicsPipeline> Shadow;
        Ref<PipelineLayout> ShadowLayout;

        // Builds every debug-blit pipeline over the shared fullscreen vertex stage, writing the
        // output format. shadowBlitSetLayout is the shadow system's dedicated blit set the shadow
        // blit samples raw depth through.
        static Unique<DebugBlitPipelines>
        Create(Context& context, AssetManager& assets, Format outputFormat,
               const Ref<DescriptorSetLayout>& shadowBlitSetLayout);
    };

    Unique<SceneRenderer::DebugBlitPipelines>
    SceneRenderer::DebugBlitPipelines::Create(Context& context, AssetManager& assets,
                                              const Format outputFormat,
                                              const Ref<DescriptorSetLayout>& shadowBlitSetLayout)
    {
        auto LoadShader = [&](const AssetId id, const char* what) -> AssetHandle<Veng::Shader>
        {
            const AssetResult<AssetHandle<Veng::Shader>> result = assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(result.has_value(), "SceneRenderer: {} shader load failed: {}", what,
                      result.error().Detail);
            return *result;
        };

        const AssetHandle<Veng::Shader> vs = LoadShader(FullscreenVertId, "fullscreen vertex");

        // Builds a fullscreen pipeline (shared vertex stage) over a layout, writing the output format.
        auto MakePipeline = [&](const char* name, const Ref<PipelineLayout>& layout,
                                const AssetHandle<Veng::Shader>& fs) -> Ref<GraphicsPipeline>
        {
            return GraphicsPipeline::Create(
                context, {
                             .Name = name,
                             .ColorAttachments = {{.Format = outputFormat}},
                             .PipelineLayout = layout,
                             .ShaderStages =
                                 {
                                     {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                                     {.Stage = ShaderStage::Fragment, .Module = fs.Get()->Module},
                                 },
                         });
        };

        // The g-buffer debug blits share the BlitPushConstants layout; only the fragment differs.
        const PushConstantRange blitRange =
            PushConstantRange::Of<BlitPushConstants>(ShaderStage::Fragment);
        auto MakeBlitLayout = [&](const char* name) -> Ref<PipelineLayout>
        {
            return PipelineLayout::Create(context,
                                          {.Name = name, .PushConstantRanges = {blitRange}});
        };

        Unique<DebugBlitPipelines> blits = CreateUnique<DebugBlitPipelines>();

        blits->AlbedoLayout = MakeBlitLayout("SceneRenderer Albedo Blit Layout");
        blits->Albedo = MakePipeline("SceneRenderer Albedo Blit Pipeline", blits->AlbedoLayout,
                                     LoadShader(AlbedoBlitFragId, "albedo-blit fragment"));

        blits->NormalLayout = MakeBlitLayout("SceneRenderer Normal Blit Layout");
        blits->Normal = MakePipeline("SceneRenderer Normal Blit Pipeline", blits->NormalLayout,
                                     LoadShader(NormalBlitFragId, "normal-blit fragment"));

        blits->DepthLayout = MakeBlitLayout("SceneRenderer Depth Blit Layout");
        blits->Depth = MakePipeline("SceneRenderer Depth Blit Pipeline", blits->DepthLayout,
                                    LoadShader(DepthBlitFragId, "depth-blit fragment"));

        blits->AoLayout = MakeBlitLayout("SceneRenderer AO Blit Layout");
        blits->Ao = MakePipeline("SceneRenderer AO Blit Pipeline", blits->AoLayout,
                                 LoadShader(AoBlitFragId, "AO-blit fragment"));

        // Motion-vector blit: samples the velocity target through the same texture+sampler push.
        blits->MotionLayout = MakeBlitLayout("SceneRenderer Motion Blit Layout");
        blits->Motion = MakePipeline("SceneRenderer Motion Blit Pipeline", blits->MotionLayout,
                                     LoadShader(MotionBlitFragId, "motion-vector-blit fragment"));

        // Shadow blit reads raw depth through a dedicated set 1, not bindless, so its layout carries
        // that set and no push block.
        blits->ShadowLayout =
            PipelineLayout::Create(context, {
                                                .Name = "SceneRenderer Shadow Blit Layout",
                                                .DescriptorSetLayouts = {shadowBlitSetLayout},
                                            });
        blits->Shadow = MakePipeline("SceneRenderer Shadow Blit Pipeline", blits->ShadowLayout,
                                     LoadShader(ShadowBlitFragId, "shadow-blit fragment"));

        // The channel select for the ORM blit is a push value, not a separate pipeline.
        blits->OrmLayout = PipelineLayout::Create(
            context, {
                         .Name = "SceneRenderer ORM Blit Layout",
                         .PushConstantRanges = {PushConstantRange::Of<OrmBlitPushConstants>(
                             ShaderStage::Fragment)},
                     });
        blits->Orm = MakePipeline("SceneRenderer ORM Blit Pipeline", blits->OrmLayout,
                                  LoadShader(OrmBlitFragId, "ORM-blit fragment"));

        return blits;
    }

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

    // Loads the two LTC lookup tables (RGBA32F, LtcLut::Size²) from the baked core-pack Raw asset
    // and uploads them into textures registered into bindless. The fit is a fixed GGX-only constant
    // baked offline (data/ltc_lut.bin: the matrix table then the magnitude table), so the runtime
    // pays no fit — just a small synchronous load and upload at setup.
    void SceneRenderer::CreateLtcResources()
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        const u32 size = LtcLut::Size;
        const usize tableBytes = static_cast<usize>(size) * size * sizeof(vec4);

        // The Raw blob is the matrix table immediately followed by the magnitude table.
        std::span<const u8> matBytes;
        std::span<const u8> magBytes;
        const AssetResult<AssetHandle<RawAsset>> lut = m_Assets.LoadSync<RawAsset>(LtcLutId);
        VE_ASSERT(lut.has_value(), "SceneRenderer: failed to load the baked LTC lookup asset");
        const vector<u8>& blob = lut.value().Get()->Bytes;
        VE_ASSERT(blob.size() >= 2 * tableBytes,
                  "SceneRenderer: LTC lookup asset is {} bytes, expected at least {}", blob.size(),
                  2 * tableBytes);
        matBytes = std::span<const u8>(blob.data(), tableBytes);
        magBytes = std::span<const u8>(blob.data() + tableBytes, tableBytes);

        m_LtcMatImage =
            Image::Create(m_Context, {
                                         .Name = "SceneRenderer LTC Matrix LUT",
                                         .Extent = {size, size, 1},
                                         .Format = Format::RGBA32Sfloat,
                                         .Usage = ImageUsage::Sampled | ImageUsage::TransferDst,
                                     });
        m_LtcMatImage->UploadSync(matBytes);
        m_LtcMatView = ImageView::Create(
            m_Context, {.Name = "SceneRenderer LTC Matrix LUT View", .Image = m_LtcMatImage});
        m_LtcMatHandle = bindless.Register(m_LtcMatView);

        m_LtcMagImage =
            Image::Create(m_Context, {
                                         .Name = "SceneRenderer LTC Magnitude LUT",
                                         .Extent = {size, size, 1},
                                         .Format = Format::RGBA32Sfloat,
                                         .Usage = ImageUsage::Sampled | ImageUsage::TransferDst,
                                     });
        m_LtcMagImage->UploadSync(magBytes);
        m_LtcMagView = ImageView::Create(
            m_Context, {.Name = "SceneRenderer LTC Magnitude LUT View", .Image = m_LtcMagImage});
        m_LtcMagHandle = bindless.Register(m_LtcMagView);
    }

    void SceneRenderer::CreatePipelines()
    {
        auto LoadShader = [this](const AssetId id, const char* what) -> AssetHandle<Veng::Shader>
        {
            const AssetResult<AssetHandle<Veng::Shader>> result =
                m_Assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(result.has_value(), "SceneRenderer: {} shader load failed: {}", what,
                      result.error().Detail);
            return *result;
        };

        const AssetHandle<Veng::Shader> vs = LoadShader(FullscreenVertId, "fullscreen vertex");
        const AssetHandle<Veng::Shader> lightingFs =
            LoadShader(DeferredLightingFragId, "deferred-lighting fragment");
        const AssetHandle<Veng::Shader> ssaoLightingFs =
            LoadShader(DeferredLightingSsaoFragId, "deferred-lighting SSAO fragment");
        const AssetHandle<Veng::Shader> cascadeDebugFs =
            LoadShader(DeferredLightingCascadesFragId, "deferred-lighting cascade-debug fragment");
        const AssetHandle<Veng::Shader> ssaoFs = LoadShader(SsaoFragId, "SSAO fragment");

        // Builds a fullscreen pipeline (shared vertex stage) over a layout, naming the
        // color-target format the pass writes.
        auto MakePipeline = [&](const char* name, const Ref<PipelineLayout>& layout,
                                const AssetHandle<Veng::Shader>& fs,
                                const Format format) -> Ref<GraphicsPipeline>
        {
            return GraphicsPipeline::Create(
                m_Context, {
                               .Name = name,
                               .ColorAttachments = {{.Format = format}},
                               .PipelineLayout = layout,
                               .ShaderStages =
                                   {
                                       {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                                       {.Stage = ShaderStage::Fragment, .Module = fs.Get()->Module},
                                   },
                           });
        };

        // Both lighting layouts carry set 1 (the shadow system: atlas + immutable
        // comparison sampler + ShadowConstants dynamic uniform) and set 2 (the IBL maps +
        // sampler). Set 0 is the reserved registry slot prepended by PipelineLayout, so the
        // shadow set is index 1 and the IBL set index 2.
        m_LightingLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Lighting Layout",
                           .DescriptorSetLayouts = {m_Shadows->GetSetLayout(),
                                                    m_SkyResolver->GetIbl().GetSetLayout()},
                           .PushConstantRanges = {PushConstantRange::Of<LightingPushConstants>(
                               ShaderStage::Fragment)},
                       });
        m_LightingPipeline = MakePipeline("SceneRenderer Deferred Lighting Pipeline",
                                          m_LightingLayout, lightingFs, HdrFormat);

        // SSAO-enabled lighting variant: wider push block (adds the AO slot) and
        // the AO-fold fragment shader. Same set-1 shadow + set-2 IBL layout.
        m_SsaoLightingLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer SSAO Lighting Layout",
                           .DescriptorSetLayouts = {m_Shadows->GetSetLayout(),
                                                    m_SkyResolver->GetIbl().GetSetLayout()},
                           .PushConstantRanges = {PushConstantRange::Of<SsaoLightingPushConstants>(
                               ShaderStage::Fragment)},
                       });
        m_SsaoLightingPipeline = MakePipeline("SceneRenderer Deferred Lighting SSAO Pipeline",
                                              m_SsaoLightingLayout, ssaoLightingFs, HdrFormat);

        // Cascade-debug variant reuses the plain lighting layout but writes the output
        // format directly — a terminal debug arm with no tonemap tail.
        m_CascadeDebugPipeline = MakePipeline("SceneRenderer Cascade Debug Pipeline",
                                              m_LightingLayout, cascadeDebugFs, m_OutputFormat);

        // Skybox: a fullscreen pass compositing the radiance cube over the lit HDR. It reads the
        // IBL set (set 1, radiance + sampler) and the depth target through bindless; its push is
        // eight u32s (Size = 32 matches SkyboxPushConstants).
        const AssetHandle<Veng::Shader> skyboxFs = LoadShader(SkyboxFragId, "skybox fragment");
        m_SkyboxLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Skybox Layout",
                           .DescriptorSetLayouts = {m_SkyResolver->GetIbl().GetSetLayout()},
                           .PushConstantRanges = {PushConstantRange{
                               .Stages = ShaderStage::Fragment, .Offset = 0, .Size = 32}},
                       });
        m_SkyboxPipeline =
            MakePipeline("SceneRenderer Skybox Pipeline", m_SkyboxLayout, skyboxFs, HdrFormat);

        // Procedural atmosphere sky: a fullscreen pass sampling the precomputed LUTs along each
        // view ray. It reads the atmosphere set (set 1, scattering + transmittance + sampler) and
        // the depth target through bindless; its push is 128 bytes (matches SkyScenePass's
        // SkyPushConstants: the 32-byte header + the 96-byte AtmosphereParams block).
        const AssetHandle<Veng::Shader> skyFs =
            LoadShader(AtmosphereSkyFragId, "atmosphere sky fragment");
        m_SkyLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Sky Layout",
                           .DescriptorSetLayouts = {m_SkyResolver->GetAtmosphere().GetSetLayout()},
                           .PushConstantRanges = {PushConstantRange{
                               .Stages = ShaderStage::Fragment, .Offset = 0, .Size = 128}},
                       });
        m_SkyPipeline = MakePipeline("SceneRenderer Sky Pipeline", m_SkyLayout, skyFs, HdrFormat);

        // Push block is eight u32s (g-buffer slots + extent); Size = 32 matches SsaoPushConstants.
        m_SsaoLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer SSAO Layout",
                           .PushConstantRanges = {PushConstantRange{
                               .Stages = ShaderStage::Fragment, .Offset = 0, .Size = 32}},
                       });
        m_SsaoPipeline =
            MakePipeline("SceneRenderer SSAO Pipeline", m_SsaoLayout, ssaoFs, SsaoFormat);

        // Loaded resident so the PostProcessScenePass builds its pipeline against the output format.
        // The tonemap material's cooked zero-override default instance — its parent supplies the
        // PostProcess pipeline and exposed schema.
        const AssetResult<AssetHandle<MaterialInstance>> tonemap =
            m_Assets.LoadSync<MaterialInstance>(TonemapInstanceId);
        VE_ASSERT(tonemap.has_value(), "SceneRenderer: tonemap material load failed: {}",
                  tonemap.error().Detail);

        // Build a per-renderer instance over the shared tonemap parent rather than using the
        // cached shared instance. The tonemap pass writes per-viewport state — the HDR source
        // handle and this frame's exposure/render-scale — into the instance's single param-block
        // slot every Execute. That slot rings by frame-in-flight, not per view, so two viewports
        // rendering in one frame through one shared instance clobber each other: the last writer's
        // source handle wins, and every earlier viewport's tonemap samples the wrong (often
        // not-yet-written) HDR. A distinct instance per renderer gives each its own slot.
        m_TonemapMaterial = m_Assets.BuildSync<MaterialInstance>(MaterialInstanceInfo{
            .Name = "SceneRenderer Tonemap",
            .Context = &m_Context,
            .Parent = tonemap->Get()->GetParent(),
            .Overrides = {},
        });

        // The fullscreen debug-blit pipelines (albedo/normal/depth/ORM/SSAO/motion/shadow) each
        // non-Final DebugView arm selects, folded into one aggregate.
        m_DebugBlits = DebugBlitPipelines::Create(m_Context, m_Assets, m_OutputFormat,
                                                  m_Shadows->GetBlitSetLayout());
    }

    void SceneRenderer::CreateOutput()
    {
        // TransferSrc for the smoke path Download(); Sampled for the composite consumer.
        // Single-copy: the consumer samples GetOutput() in the same frame it is written.
        m_OutputImage =
            Image::Create(m_Context, {
                                         .Name = "SceneRenderer Output",
                                         .Extent = {m_Extent.x, m_Extent.y, 1},
                                         .Format = m_OutputFormat,
                                         .Usage = ImageUsage::ColorAttachment |
                                                  ImageUsage::Sampled | ImageUsage::TransferSrc,
                                     });

        m_OutputView = ImageView::Create(m_Context, {
                                                        .Name = "SceneRenderer Output View",
                                                        .Image = m_OutputImage,
                                                    });
    }

    void SceneRenderer::CreateGBuffer()
    {
        // Renderer-owned and Imported (not a graph transient — sampled downstream through bindless).
        // Releasing old slots defers through the same per-frame window as the images retire.
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_AlbedoHandle);
        bindless.Release(m_NormalHandle);
        bindless.Release(m_OrmHandle);
        bindless.Release(m_DepthHandle);
        bindless.Release(m_VelocityHandle);
        bindless.Release(m_EmissiveHandle);

        m_AlbedoImage = Image::Create(m_Context, {
                                                     .Name = "SceneRenderer GBuffer Albedo",
                                                     .Extent = {m_Extent.x, m_Extent.y, 1},
                                                     .Format = GBuffer::AlbedoFormat,
                                                     .Usage = GBuffer::ColorUsage,
                                                 });
        m_AlbedoView = ImageView::Create(
            m_Context, {.Name = "SceneRenderer GBuffer Albedo View", .Image = m_AlbedoImage});

        m_NormalImage = Image::Create(m_Context, {
                                                     .Name = "SceneRenderer GBuffer Normal",
                                                     .Extent = {m_Extent.x, m_Extent.y, 1},
                                                     .Format = GBuffer::NormalFormat,
                                                     .Usage = GBuffer::ColorUsage,
                                                 });
        m_NormalView = ImageView::Create(
            m_Context, {.Name = "SceneRenderer GBuffer Normal View", .Image = m_NormalImage});

        m_OrmImage = Image::Create(m_Context, {
                                                  .Name = "SceneRenderer GBuffer ORM",
                                                  .Extent = {m_Extent.x, m_Extent.y, 1},
                                                  .Format = GBuffer::ORMFormat,
                                                  .Usage = GBuffer::ColorUsage,
                                              });
        m_OrmView = ImageView::Create(
            m_Context, {.Name = "SceneRenderer GBuffer ORM View", .Image = m_OrmImage});

        m_DepthImage = Image::Create(m_Context, {
                                                    .Name = "SceneRenderer GBuffer Depth",
                                                    .Extent = {m_Extent.x, m_Extent.y, 1},
                                                    .Format = GBuffer::DepthFormat,
                                                    .Usage = GBuffer::DepthUsage,
                                                });
        m_DepthView = ImageView::Create(
            m_Context, {.Name = "SceneRenderer GBuffer Depth View", .Image = m_DepthImage});

        // G3 — the per-object screen-space motion vector. A g-buffer channel like the others:
        // the surface pass writes it as SV_Target3 every frame (the TAA resolve and the
        // MotionVectors debug blit read it), so it is always allocated, not TAA-gated.
        m_VelocityImage = Image::Create(m_Context, {
                                                       .Name = "SceneRenderer GBuffer Velocity",
                                                       .Extent = {m_Extent.x, m_Extent.y, 1},
                                                       .Format = GBuffer::VelocityFormat,
                                                       .Usage = GBuffer::ColorUsage,
                                                   });
        m_VelocityView = ImageView::Create(
            m_Context, {.Name = "SceneRenderer GBuffer Velocity View", .Image = m_VelocityImage});

        // G4 — the HDR emissive channel. A g-buffer channel like the others: the surface pass
        // writes authored emission as SV_Target4 every frame, so it is always allocated, and the
        // lighting pass samples it to add emission into the outgoing radiance.
        m_EmissiveImage = Image::Create(m_Context, {
                                                       .Name = "SceneRenderer GBuffer Emissive",
                                                       .Extent = {m_Extent.x, m_Extent.y, 1},
                                                       .Format = GBuffer::EmissiveFormat,
                                                       .Usage = GBuffer::ColorUsage,
                                                   });
        m_EmissiveView = ImageView::Create(
            m_Context, {.Name = "SceneRenderer GBuffer Emissive View", .Image = m_EmissiveImage});

        if (!m_Sampler)
        {
            m_Sampler = Sampler::Create(m_Context, {
                                                       .Name = "SceneRenderer GBuffer Sampler",
                                                       .AddressModeU = AddressMode::ClampToEdge,
                                                       .AddressModeV = AddressMode::ClampToEdge,
                                                       .AddressModeW = AddressMode::ClampToEdge,
                                                   });
            m_SamplerHandle = bindless.Register(m_Sampler);
        }

        m_AlbedoHandle = bindless.Register(m_AlbedoView);
        m_NormalHandle = bindless.Register(m_NormalView);
        m_OrmHandle = bindless.Register(m_OrmView);
        m_DepthHandle = bindless.Register(m_DepthView);
        m_VelocityHandle = bindless.Register(m_VelocityView);
        m_EmissiveHandle = bindless.Register(m_EmissiveView);

        // The hi-Z pyramid's reduction sets bind the fresh depth view, so it is (re)built from the
        // g-buffer create/recreate tail.
        m_GpuCull->ResizeHiZ(m_Extent, m_DepthView);
    }

    void SceneRenderer::CreateCullResources()
    {
        // The per-draw DrawData SSBO drives both cull modes' buffer-indexed draw. Host-visible,
        // ring-buffered for frames-in-flight; the surface vertex stage indexes it by the pushed
        // FrameBase folded with the candidate id.
        const u64 drawDataRegion = static_cast<u64>(MaxCullCandidates) * sizeof(GpuDrawData);
        m_DrawDataBuffer = Buffer::Create(m_Context, {
                                                         .Name = "SceneRenderer DrawData",
                                                         .Size = drawDataRegion * m_FramesInFlight,
                                                         .Usage = BufferUsage::Storage,
                                                         .HostMapped = true,
                                                     });

        // Stage flags must match the surface pipeline's reflected set-1 layout exactly for
        // descriptor-set compatibility. The shared material header declares g_DrawData in
        // both stages (the fragment includes it even though only the vertex stage reads it),
        // so the cooker reflects it Vertex | Fragment — match that here.
        m_DrawDataSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer DrawData Set Layout",
                           .Bindings = {{.Binding = 0,
                                         .Type = DescriptorType::StorageBuffer,
                                         .Count = 1,
                                         .Stages = ShaderStage::Vertex | ShaderStage::Fragment}},
                       });
        m_DrawDataSet = DescriptorSet::Create(m_Context, {
                                                             .Name = "SceneRenderer DrawData Set",
                                                             .Layout = m_DrawDataSetLayout,
                                                         });
        m_DrawDataSet->Write(0, m_DrawDataBuffer);

        // The per-instance skinning palette drives skinned draws. Host-visible, ring-buffered;
        // a skinned draw's DrawData.PaletteBase indexes it directly. Vertex-stage only — the
        // skinned vertex shaders declare g_Palette in the vertex stage alone, so the reflected
        // set layout (set 2 for the surface pipeline, set 1 for the shadow pipeline) is Vertex.
        const u64 paletteRegion = static_cast<u64>(MaxSkinningMatricesPerFrame) * sizeof(mat4);
        m_PaletteBuffer = Buffer::Create(m_Context, {
                                                        .Name = "SceneRenderer Skinning Palette",
                                                        .Size = paletteRegion * m_FramesInFlight,
                                                        .Usage = BufferUsage::Storage,
                                                        .HostMapped = true,
                                                    });
        m_PaletteSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Palette Set Layout",
                           .Bindings = {{.Binding = 0,
                                         .Type = DescriptorType::StorageBuffer,
                                         .Count = 1,
                                         .Stages = ShaderStage::Vertex}},
                       });
        m_PaletteSet = DescriptorSet::Create(m_Context, {
                                                            .Name = "SceneRenderer Palette Set",
                                                            .Layout = m_PaletteSetLayout,
                                                        });
        m_PaletteSet->Write(0, m_PaletteBuffer);

        // The identity candidate-id buffer bound to vertex binding 1 (instance rate): element k
        // holds k, so a draw's firstInstance = candidateId fetches candidateId as the attribute.
        vector<u32> identity(MaxCullCandidates);
        for (u32 i = 0; i < MaxCullCandidates; ++i)
        {
            identity[i] = i;
        }
        m_CandidateIdBuffer =
            Buffer::Create(m_Context, {
                                          .Name = "SceneRenderer Candidate Ids",
                                          .Size = identity.size() * sizeof(u32),
                                          .Usage = BufferUsage::Vertex | BufferUsage::TransferDst,
                                      });
        m_CandidateIdBuffer->UploadSync(std::span<const u8>(
            reinterpret_cast<const u8*>(identity.data()), identity.size() * sizeof(u32)));
    }

    void SceneRenderer::CreateHdr()
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_HdrHandle);

        m_HdrImage = Image::Create(m_Context, {
                                                  .Name = "SceneRenderer HDR",
                                                  .Extent = {m_Extent.x, m_Extent.y, 1},
                                                  .Format = HdrFormat,
                                                  .Usage = HdrUsage,
                                              });
        m_HdrView =
            ImageView::Create(m_Context, {.Name = "SceneRenderer HDR View", .Image = m_HdrImage});

        m_HdrHandle = bindless.Register(m_HdrView);
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
        const ResourceId sceneColorId = ssrActive ? m_SsrSceneId : hdrId;
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
        const TextureHandle lightingTargetHandle =
            taaActive ? m_Taa->GetLitHandle() : (ssrActive ? m_Ssr->GetSceneHandle() : m_HdrHandle);

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
                                   m_NormalId, m_OrmId, m_DepthId, m_HdrId, m_DepthHandle,
                                   m_NormalHandle, m_OrmHandle, m_AlbedoHandle, m_SamplerHandle);
                }
                if (m_PointFieldPass != nullptr)
                {
                    // The fields write the final HDR target, not the pre-TAA/SSR lit target the
                    // in-list passes see as io.Hdr.
                    PassIO fieldIo = io;
                    fieldIo.Hdr = m_HdrId;
                    m_PointFieldPass->Declare(graph, fieldIo);
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
        } debugDrawClearGuard{m_DebugDraw};

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
