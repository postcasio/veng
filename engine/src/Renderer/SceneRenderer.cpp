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
#include "Passes/AtmospherePrecompute.h"
#include "Passes/DebugBlitScenePasses.h"
#include "Passes/DeferredLightingScenePass.h"
#include "Passes/GBufferScenePass.h"
#include "Passes/PickingScenePass.h"
#include "Passes/PointFieldScenePass.h"
#include "Passes/SceneColorCopyScenePass.h"
#include "Passes/SkyScenePass.h"
#include "Passes/TaaScenePass.h"
#include "Passes/TranslucentScenePass.h"
#include "Passes/VolumeScenePass.h"
#include "ShadowScenePass.h"
#include "PunctualShadowScenePass.h"
#include "ShadowSystem.h"
#include "Picking.h"
#include "SkyboxScenePass.h"
#include "SkyCubemapBake.h"
#include "SsaoScenePass.h"
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

        // Cube face edge length for the baked material sky. Mip 0 suffices for display; the
        // roughness chain the IBL tier needs is convolved from this cube, not baked here. Sized so
        // a point feature a sky material bakes (a star) subtends only a few display pixels — at 512
        // a single face texel already covers ~7 pixels of a 1440p view, reading as a blob.
        constexpr u32 SkyBakeFaceSize = 1024;

        // Field-wise equality of two Atmosphere parameter sets; gates the once-per-change LUT
        // regeneration. An exact compare is right — the LUTs are a pure function of these fields,
        // so any bit change must regenerate and an unchanged set must not.
        bool AtmosphereEquals(const Atmosphere& a, const Atmosphere& b)
        {
            return a.RayleighScattering == b.RayleighScattering &&
                   a.RayleighHeight == b.RayleighHeight && a.MieScattering == b.MieScattering &&
                   a.MieExtinction == b.MieExtinction && a.MieHeight == b.MieHeight &&
                   a.MieAnisotropy == b.MieAnisotropy && a.OzoneAbsorption == b.OzoneAbsorption &&
                   a.OzoneCenter == b.OzoneCenter && a.OzoneWidth == b.OzoneWidth &&
                   a.PlanetRadius == b.PlanetRadius && a.AtmosphereRadius == b.AtmosphereRadius &&
                   a.SunAngularRadius == b.SunAngularRadius && a.SunIrradiance == b.SunIrradiance;
        }

        constexpr AssetId SsaoFragId{0xCCBA63DB760A4E8EULL};
        constexpr AssetId TonemapInstanceId{0xB5AA7227E8A2DC11ULL};
        constexpr AssetId AlbedoBlitFragId{0xF90F709155D04BE7ULL};
        constexpr AssetId NormalBlitFragId{0x5A2CD7B270EAE5CDULL};
        constexpr AssetId DepthBlitFragId{0xE05F5F86E72F96D5ULL};
        constexpr AssetId OrmBlitFragId{0x7992B54A844CB1E1ULL};
        constexpr AssetId AoBlitFragId{0x97974B40192934E4ULL};
        constexpr AssetId MotionBlitFragId{0xCCD40C76935382FDULL};
        constexpr AssetId ShadowBlitFragId{0x0B61D5D42DAEF190ULL};

        // The entity-id picking shaders: a minimal vertex stage (static + skinned) that emits the
        // per-draw entity index, and the fragment that writes index + 1 into the EntityId target.
        constexpr AssetId EntityIdFragId{0xBE08B2489A5AA07AULL};
        constexpr AssetId EntityIdVertId{0xE21B8F492DADABE5ULL};
        constexpr AssetId EntityIdSkinnedVertId{0x7FB330D3ABACAE0FULL};

        // The refraction scene-color copy fragment shader.
        constexpr AssetId SceneColorCopyFragId{0xBE7002B7B8E9BE5AULL};

        // The hi-Z max-Z reduction compute shader.
        constexpr AssetId HiZReduceCompId{0xCB20C4EF8A20ADBCULL};

        // The GPU occlusion-cull → indirect-draw compute shader.
        constexpr AssetId OcclusionCullCompId{0x5FE19B500FD44B52ULL};

        constexpr AssetId SsrTraceFragId{0xBDBD7BC71B2B1E74ULL};
        constexpr AssetId SsrBlurDownCompId{0xEE0EED485023A7F6ULL};
        constexpr AssetId SsrCompositeFragId{0x50D9ECEAE45E31A1ULL};
        constexpr AssetId SsrHiZReduceCompId{0x93DA6E42B3B5479AULL};

        // Linear float HDR format for the lighting target and the tail's scene-color intermediates.
        // G1 uses the same format as a sampled color target, establishing RGBA16F
        // color-attachment + sampled support on the platform.
        constexpr Format HdrFormat = Format::RGBA16Sfloat;
        constexpr ImageUsage HdrUsage = ImageUsage::ColorAttachment | ImageUsage::Sampled;

        // The SSR reflection mip chain stops this many levels short of 1×1 — a rough reflection
        // needs no 1-px mip. The same pyramid-depth heuristic the bloom pyramid uses internally.
        constexpr u32 BloomTileShift = 3;

        // Single-channel unorm format for the SSAO target; the renderer builds the
        // SSAO pipeline against this format, and SsaoScenePass owns the image.
        constexpr Format SsaoFormat = Format::R8Unorm;

        // Single-channel float for the hi-Z pyramid; the reduction stores max depth.
        constexpr Format HiZFormat = Format::R32Sfloat;

        // The hi-Z reduction push block: the destination and source mip extents, so a
        // boundary invocation skips out-of-range texels and an odd parent dimension
        // folds its dropped row/column into the max (matches hi_z_reduce.comp).
        struct HiZReducePush
        {
            uvec2 DestExtent;
            uvec2 SourceExtent;
        };

        // The SSR trace push block, matching ssr_trace.frag PushConstants: the scene-color
        // and g-buffer bindless slots, the shared sampler, the view-constants region, the
        // reflection extent, and the ray parameters (max distance, hit thickness, roughness
        // cutoff, step count).
        struct SsrTracePush
        {
            u32 SceneColorTexture;
            u32 DepthTexture;
            u32 NormalTexture;
            u32 OrmTexture;
            u32 HiZTexture;
            u32 Sampler;
            u32 ViewConstantsIndex;
            u32 HiZLevels;
            uvec2 Extent;
            f32 MaxDistance;
            f32 Thickness;
            f32 MaxRoughness;
            u32 MaxSteps;
        };

        // The SSR reflection blur-downsample push, matching ssr_blur_down.comp: the
        // destination mip extent.
        struct SsrBlurPush
        {
            uvec2 DestExtent;
        };

        // The SSR composite push, matching ssr_composite.frag PushConstants: the scene-color,
        // reflection, and g-buffer slots, the shared + reflection samplers, the view-constants
        // region, the reflection mix, and the reflection mip count (roughness → LOD).
        struct SsrCompositePush
        {
            u32 SceneColorTexture;
            u32 ReflectionTexture;
            u32 AlbedoTexture;
            u32 NormalTexture;
            u32 OrmTexture;
            u32 DepthTexture;
            u32 Sampler;
            u32 ReflectionSampler;
            u32 ViewConstantsIndex;
            f32 Intensity;
            u32 MipCount;
            u32 Pad0;
        };

        // The cull compute push block, matching occlusion_cull.comp PushConstants.
        struct OcclusionCullPush
        {
            mat4 PrevViewProj;
            uvec2 HiZBaseExtent;
            u32 CandidateCount;
            u32 HistoryValid;
            f32 DepthBias;
            u32 FrameBase;
            u32 CountIndex;
        };
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
        ResolveActiveCullMode();
        // Frames-in-flight is spine — the renderer's own rings (draw data, palette, cull) size
        // from it, so seed it before those are allocated below. Each subsystem derives its own
        // ring depth from the context independently.
        m_FramesInFlight = m_Context.GetMaxFramesInFlight();
        m_Shadows = ShadowSystem::Create(m_Context, m_Settings);
        // The IBL maps + their consumer set layout exist before the pipelines so the lighting
        // layout can reserve the set (set 2).
        m_Ibl = EnvironmentIbl::Create(m_Context, m_Assets);
        // The atmosphere LUTs + their consumer set layout exist before the pipelines so the sky
        // layout can reserve the set (set 1).
        m_Atmosphere = AtmospherePrecompute::Create(m_Context, m_Assets);
        // The sky-material bake owns its radiance cube + a consumer set matching the IBL radiance
        // binding, so a baked material sky samples through the same skybox pipeline. The cube renders
        // at HdrFormat (the scene-color format) so its radiance round-trips the skybox sampler.
        m_SkyBake =
            SkyCubemapBake::Create(m_Context, m_Ibl->GetSetLayout(), HdrFormat, SkyBakeFaceSize);
        // Bloom is constructed before the pipelines because its down/up set layout is reserved by
        // the SSR blur pipeline layout CreatePipelines builds; its extent-sized pyramid is built by
        // Resize below (after the HDR target). TAA is grouped with it; both build their pipelines
        // in their constructors.
        m_Bloom = BloomPyramid::Create(m_Context, m_Assets, m_Settings.Kernel);
        m_Taa = TaaResolve::Create(m_Context, m_Assets);
        CreatePipelines();

        CreateOutput();
        CreateGBuffer();
        CreateLtcResources();
        CreateCullResources();
        CreateHdr();
        m_Taa->Resize(m_Extent, m_Settings.TAA);
        // The pyramid's level-0 source and composite sets bind the fresh HDR view.
        m_Bloom->Resize(m_Extent, m_HdrView);
        CreateSsr();
        CreateRefraction();
        // The metering set binds the HDR target, so the meter is created after CreateHdr.
        m_AutoExposure = AutoExposureMeter::Create(m_Context, m_Assets, m_HdrView);
        CreatePicking();
        Rebuild();
    }

    SceneRenderer::~SceneRenderer()
    {
        // Release bindless slots before their images retire.
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_AlbedoHandle);
        bindless.Release(m_NormalHandle);
        bindless.Release(m_OrmHandle);
        bindless.Release(m_DepthHandle);
        bindless.Release(m_HiZSampleHandle);
        bindless.Release(m_HdrHandle);
        bindless.Release(m_VelocityHandle);
        bindless.Release(m_EmissiveHandle);
        bindless.Release(m_SsrSceneHandle);
        bindless.Release(m_SsrReflectionSampleHandle);
        bindless.Release(m_SsrReflectionSamplerHandle);
        bindless.Release(m_SsrHiZSampleHandle);
        bindless.Release(m_RefractionSceneHandle);
        bindless.Release(m_RefractionDepthHandle);
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
        const AssetHandle<Veng::Shader> albedoBlitFs =
            LoadShader(AlbedoBlitFragId, "albedo-blit fragment");
        const AssetHandle<Veng::Shader> normalBlitFs =
            LoadShader(NormalBlitFragId, "normal-blit fragment");
        const AssetHandle<Veng::Shader> depthBlitFs =
            LoadShader(DepthBlitFragId, "depth-blit fragment");
        const AssetHandle<Veng::Shader> ormBlitFs = LoadShader(OrmBlitFragId, "ORM-blit fragment");
        const AssetHandle<Veng::Shader> aoBlitFs = LoadShader(AoBlitFragId, "AO-blit fragment");
        const AssetHandle<Veng::Shader> motionBlitFs =
            LoadShader(MotionBlitFragId, "motion-vector-blit fragment");
        const AssetHandle<Veng::Shader> shadowBlitFs =
            LoadShader(ShadowBlitFragId, "shadow-blit fragment");

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
            m_Context,
            {
                .Name = "SceneRenderer Lighting Layout",
                .DescriptorSetLayouts = {m_Shadows->GetSetLayout(), m_Ibl->GetSetLayout()},
                .PushConstantRanges = {PushConstantRange::Of<LightingPushConstants>(
                    ShaderStage::Fragment)},
            });
        m_LightingPipeline = MakePipeline("SceneRenderer Deferred Lighting Pipeline",
                                          m_LightingLayout, lightingFs, HdrFormat);

        // SSAO-enabled lighting variant: wider push block (adds the AO slot) and
        // the AO-fold fragment shader. Same set-1 shadow + set-2 IBL layout.
        m_SsaoLightingLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "SceneRenderer SSAO Lighting Layout",
                .DescriptorSetLayouts = {m_Shadows->GetSetLayout(), m_Ibl->GetSetLayout()},
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
                           .DescriptorSetLayouts = {m_Ibl->GetSetLayout()},
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
                           .DescriptorSetLayouts = {m_Atmosphere->GetSetLayout()},
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

        // The refraction scene-color copy writes the intermediate translucent materials sample.
        const AssetHandle<Veng::Shader> sceneColorCopyFs =
            LoadShader(SceneColorCopyFragId, "scene-color copy fragment");
        m_SceneColorCopyLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Scene Color Copy Layout",
                           .PushConstantRanges = {PushConstantRange::Of<SceneColorCopyPush>(
                               ShaderStage::Fragment)},
                       });
        // Two attachments (the scene-color grab + the depth copy), so the single-format
        // MakePipeline convenience does not apply.
        m_SceneColorCopyPipeline = GraphicsPipeline::Create(
            m_Context,
            {
                .Name = "SceneRenderer Scene Color Copy Pipeline",
                .ColorAttachments = {{.Format = HdrFormat}, {.Format = Format::R32Sfloat}},
                .PipelineLayout = m_SceneColorCopyLayout,
                .ShaderStages =
                    {
                        {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                        {.Stage = ShaderStage::Fragment, .Module = sceneColorCopyFs.Get()->Module},
                    },
            });

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

        // The g-buffer debug blits share the BlitPushConstants layout; only the
        // fragment shader differs.
        const PushConstantRange blitRange =
            PushConstantRange::Of<BlitPushConstants>(ShaderStage::Fragment);

        m_AlbedoBlitLayout =
            PipelineLayout::Create(m_Context, {
                                                  .Name = "SceneRenderer Albedo Blit Layout",
                                                  .PushConstantRanges = {blitRange},
                                              });
        m_AlbedoBlitPipeline = MakePipeline("SceneRenderer Albedo Blit Pipeline",
                                            m_AlbedoBlitLayout, albedoBlitFs, m_OutputFormat);

        m_NormalBlitLayout =
            PipelineLayout::Create(m_Context, {
                                                  .Name = "SceneRenderer Normal Blit Layout",
                                                  .PushConstantRanges = {blitRange},
                                              });
        m_NormalBlitPipeline = MakePipeline("SceneRenderer Normal Blit Pipeline",
                                            m_NormalBlitLayout, normalBlitFs, m_OutputFormat);

        m_DepthBlitLayout =
            PipelineLayout::Create(m_Context, {
                                                  .Name = "SceneRenderer Depth Blit Layout",
                                                  .PushConstantRanges = {blitRange},
                                              });
        m_DepthBlitPipeline = MakePipeline("SceneRenderer Depth Blit Pipeline", m_DepthBlitLayout,
                                           depthBlitFs, m_OutputFormat);

        // AO blit: same push shape as the g-buffer blits.
        m_AoBlitLayout =
            PipelineLayout::Create(m_Context, {
                                                  .Name = "SceneRenderer AO Blit Layout",
                                                  .PushConstantRanges = {blitRange},
                                              });
        m_AoBlitPipeline = MakePipeline("SceneRenderer AO Blit Pipeline", m_AoBlitLayout, aoBlitFs,
                                        m_OutputFormat);

        // Motion-vector blit: samples the velocity target through the same texture+sampler
        // push as the g-buffer blits.
        m_MotionBlitLayout =
            PipelineLayout::Create(m_Context, {
                                                  .Name = "SceneRenderer Motion Blit Layout",
                                                  .PushConstantRanges = {blitRange},
                                              });
        m_MotionBlitPipeline = MakePipeline("SceneRenderer Motion Blit Pipeline",
                                            m_MotionBlitLayout, motionBlitFs, m_OutputFormat);

        // Shadow blit reads raw depth through a dedicated set 1, not bindless,
        // so its layout carries that set and no push block.
        m_ShadowBlitLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Shadow Blit Layout",
                           .DescriptorSetLayouts = {m_Shadows->GetBlitSetLayout()},
                       });
        m_ShadowBlitPipeline = MakePipeline("SceneRenderer Shadow Blit Pipeline",
                                            m_ShadowBlitLayout, shadowBlitFs, m_OutputFormat);

        m_OrmBlitLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer ORM Blit Layout",
                           .PushConstantRanges = {PushConstantRange::Of<OrmBlitPushConstants>(
                               ShaderStage::Fragment)},
                       });
        m_OrmBlitPipeline = MakePipeline("SceneRenderer ORM Blit Pipeline", m_OrmBlitLayout,
                                         ormBlitFs, m_OutputFormat);

        // The hi-Z reduction compute pipeline. Set 1 (binding 0 sampled source, binding 1
        // storage dest) is off bindless — a closed producer→consumer reduction needs no
        // global registration, and a dedicated set sidesteps the set-0 storage-image
        // argument-buffer path on MoltenVK.
        const AssetHandle<Veng::Shader> hiZReduceCs =
            LoadShader(HiZReduceCompId, "hi-Z reduce compute");
        m_HiZReduceSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer HiZ Reduce Set Layout",
                           .Bindings =
                               {
                                   {.Binding = 0,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 1,
                                    .Type = DescriptorType::StorageImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                               },
                       });
        m_HiZReduceLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "SceneRenderer HiZ Reduce Layout",
                .DescriptorSetLayouts = {m_HiZReduceSetLayout},
                .PushConstantRanges = {PushConstantRange::Of<HiZReducePush>(ShaderStage::Compute)},
            });
        m_HiZReducePipeline = ComputePipeline::Create(
            m_Context,
            {
                .Name = "SceneRenderer HiZ Reduce Pipeline",
                .PipelineLayout = m_HiZReduceLayout,
                .ShaderStage = {.Stage = ShaderStage::Compute, .Module = hiZReduceCs.Get()->Module},
            });

        // SSR pipelines. The trace and composite are fullscreen graphics passes reading the
        // scene/g-buffer through set-0 bindless (so set 0 is auto-reserved, no extra sets); the
        // blur reuses the bloom down/up set layout (sampled source + linear sampler + storage dest).
        const AssetHandle<Veng::Shader> ssrTraceFs =
            LoadShader(SsrTraceFragId, "SSR trace fragment");
        const AssetHandle<Veng::Shader> ssrCompositeFs =
            LoadShader(SsrCompositeFragId, "SSR composite fragment");
        const AssetHandle<Veng::Shader> ssrBlurCs =
            LoadShader(SsrBlurDownCompId, "SSR blur downsample");
        // The min-Z reduction reuses the hi-Z reduce layout/set layout (sampled source +
        // storage dest + the HiZReducePush) — only the reduce operator (min vs max) differs.
        const AssetHandle<Veng::Shader> ssrHiZReduceCs =
            LoadShader(SsrHiZReduceCompId, "SSR min-Z reduce");
        m_SsrHiZReducePipeline = ComputePipeline::Create(
            m_Context, {
                           .Name = "SceneRenderer SSR MinZ Reduce Pipeline",
                           .PipelineLayout = m_HiZReduceLayout,
                           .ShaderStage = {.Stage = ShaderStage::Compute,
                                           .Module = ssrHiZReduceCs.Get()->Module},
                       });

        m_SsrTraceLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "SceneRenderer SSR Trace Layout",
                .PushConstantRanges = {PushConstantRange::Of<SsrTracePush>(ShaderStage::Fragment)},
            });
        m_SsrTracePipeline = MakePipeline("SceneRenderer SSR Trace Pipeline", m_SsrTraceLayout,
                                          ssrTraceFs, HdrFormat);

        m_SsrCompositeLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer SSR Composite Layout",
                           .PushConstantRanges = {PushConstantRange::Of<SsrCompositePush>(
                               ShaderStage::Fragment)},
                       });
        m_SsrCompositePipeline = MakePipeline("SceneRenderer SSR Composite Pipeline",
                                              m_SsrCompositeLayout, ssrCompositeFs, HdrFormat);

        m_SsrBlurLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "SceneRenderer SSR Blur Layout",
                .DescriptorSetLayouts = {m_Bloom->GetDownUpSetLayout()},
                .PushConstantRanges = {PushConstantRange::Of<SsrBlurPush>(ShaderStage::Compute)},
            });
        m_SsrBlurPipeline = ComputePipeline::Create(
            m_Context,
            {
                .Name = "SceneRenderer SSR Blur Pipeline",
                .PipelineLayout = m_SsrBlurLayout,
                .ShaderStage = {.Stage = ShaderStage::Compute, .Module = ssrBlurCs.Get()->Module},
            });
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

    void SceneRenderer::CreatePicking()
    {
        if (!m_Settings.Picking)
        {
            // Release any previously-allocated picking resources (a Configure turning it off);
            // the pipelines are rebuilt lazily, so they are dropped here too.
            m_EntityIdImage.reset();
            m_EntityIdView.reset();
            m_PickingDepthImage.reset();
            m_PickingDepthView.reset();
            m_PickReadbackBuffer.reset();
            m_PickingPipeline.reset();
            m_PickingSkinnedPipeline.reset();
            m_PickRequested = false;
            m_PickStaged = false;
            return;
        }

        VE_ASSERT(
            m_Context.IsFormatColorAttachmentTransferSrcSupported(Picking::EntityIdFormat),
            "SceneRenderer: the device does not support R32Uint as a color attachment + transfer "
            "source, required for the entity-id picking target");

        m_EntityIdImage = Image::Create(m_Context, {
                                                       .Name = "SceneRenderer EntityId",
                                                       .Extent = {m_Extent.x, m_Extent.y, 1},
                                                       .Format = Picking::EntityIdFormat,
                                                       .Usage = Picking::EntityIdUsage,
                                                   });
        m_EntityIdView = ImageView::Create(
            m_Context, {.Name = "SceneRenderer EntityId View", .Image = m_EntityIdImage});

        m_PickingDepthImage = Image::Create(m_Context, {
                                                           .Name = "SceneRenderer Picking Depth",
                                                           .Extent = {m_Extent.x, m_Extent.y, 1},
                                                           .Format = GBuffer::DepthFormat,
                                                           .Usage = ImageUsage::DepthAttachment,
                                                       });
        m_PickingDepthView = ImageView::Create(
            m_Context, {.Name = "SceneRenderer Picking Depth View", .Image = m_PickingDepthImage});

        // The readback staging buffer: one search-window's worth of u32s. Only one pick is in
        // flight at a time (a new request is dropped until the prior resolves), so the staged copy
        // is never overwritten before the host reads it — one region suffices.
        const u32 window = static_cast<u32>(2 * Picking::SearchRadius + 1);
        m_PickReadbackStride = window * window * static_cast<u32>(sizeof(u32));
        m_PickReadbackBuffer = Buffer::Create(m_Context, {
                                                             .Name = "SceneRenderer Pick Readback",
                                                             .Size = m_PickReadbackStride,
                                                             .Usage = BufferUsage::TransferDst,
                                                             .HostMapped = true,
                                                         });

        // A resize/configure recreates the id target, so an in-flight staged copy is moot.
        m_PickStaged = false;
    }

    void SceneRenderer::EnsurePickingPipelines(const MaterialInstance* staticMaterial,
                                               const MaterialInstance* skinnedMaterial)
    {
        if (!m_Settings.Picking)
        {
            return;
        }

        // The id-writing variants reuse the surface material's pipeline layout (set 0 bindless +
        // set 1 DrawData [+ set 2 palette for skinned] + the SurfacePush) so the picking pass binds
        // the same per-draw DrawData and palette the geometry pass does. They pair the dedicated
        // entity_id vertex stages (which emit only the entity index) with the entity_id fragment,
        // and bind the EntityId target + dedicated depth instead of the g-buffer. The layout is
        // identical across all surface materials, so the first available one builds the cached pipeline.
        auto LoadShader = [this](const AssetId id, const char* what) -> AssetHandle<Veng::Shader>
        {
            const AssetResult<AssetHandle<Veng::Shader>> result =
                m_Assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(result.has_value(), "SceneRenderer: {} shader load failed: {}", what,
                      result.has_value() ? "" : result.error().Detail);
            return *result;
        };

        if (!m_PickingPipeline && staticMaterial != nullptr)
        {
            const AssetHandle<Veng::Shader> vs = LoadShader(EntityIdVertId, "entity-id vertex");
            const AssetHandle<Veng::Shader> fs = LoadShader(EntityIdFragId, "entity-id fragment");
            m_PickingPipeline = GraphicsPipeline::Create(
                m_Context, {
                               .Name = "SceneRenderer Picking Pipeline",
                               .ColorAttachments = {{.Format = Picking::EntityIdFormat}},
                               .DepthAttachmentFormat = GBuffer::DepthFormat,
                               .VertexBufferLayout = Mesh::CanonicalLayout(),
                               .InstanceCandidateId = true,
                               .PipelineLayout = staticMaterial->GetPipelineLayout(),
                               .ShaderStages =
                                   {
                                       {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                                       {.Stage = ShaderStage::Fragment, .Module = fs.Get()->Module},
                                   },
                               .CullMode = CullMode::Back,
                               .DepthTestEnable = true,
                               .DepthWriteEnable = true,
                               .DepthCompareOp = CompareOp::LessOrEqual,
                           });
        }

        if (!m_PickingSkinnedPipeline && skinnedMaterial != nullptr)
        {
            const AssetHandle<Veng::Shader> vs =
                LoadShader(EntityIdSkinnedVertId, "entity-id skinned vertex");
            const AssetHandle<Veng::Shader> fs = LoadShader(EntityIdFragId, "entity-id fragment");
            m_PickingSkinnedPipeline = GraphicsPipeline::Create(
                m_Context, {
                               .Name = "SceneRenderer Picking Skinned Pipeline",
                               .ColorAttachments = {{.Format = Picking::EntityIdFormat}},
                               .DepthAttachmentFormat = GBuffer::DepthFormat,
                               .VertexBufferLayout = Mesh::SkinnedLayout(),
                               .InstanceCandidateId = true,
                               .PipelineLayout = skinnedMaterial->GetPipelineLayout(),
                               .ShaderStages =
                                   {
                                       {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                                       {.Stage = ShaderStage::Fragment, .Module = fs.Get()->Module},
                                   },
                               .CullMode = CullMode::Back,
                               .DepthTestEnable = true,
                               .DepthWriteEnable = true,
                               .DepthCompareOp = CompareOp::LessOrEqual,
                           });
        }
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

        CreateHiZ();
    }

    void SceneRenderer::CreateHiZ()
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_HiZSampleHandle);

        // A full mip chain over the depth extent: floor(log2(max(w,h))) + 1 levels.
        const u32 maxDim = std::max(m_Extent.x, m_Extent.y);
        const u32 mipCount = maxDim == 0 ? 1 : (std::bit_width(maxDim));

        m_HiZImage =
            Image::Create(m_Context, {
                                         .Name = "SceneRenderer HiZ",
                                         .Extent = {m_Extent.x, m_Extent.y, 1},
                                         .MipLevels = mipCount,
                                         .Format = HiZFormat,
                                         .Usage = ImageUsage::Storage | ImageUsage::Sampled,
                                     });

        // One single-mip storage view per level (the reduction writes each), plus a
        // whole-chain sampled view for the occlusion test.
        m_HiZMips.clear();
        m_HiZMips.reserve(mipCount);
        for (u32 level = 0; level < mipCount; level++)
        {
            m_HiZMips.push_back(ImageView::Create(
                m_Context, {
                               .Name = fmt::format("SceneRenderer HiZ Mip {} View", level),
                               .Image = m_HiZImage,
                               .BaseMipLevel = level,
                               .MipLevels = 1,
                           }));
        }
        m_HiZSampleView = ImageView::Create(m_Context, {
                                                           .Name = "SceneRenderer HiZ Sample View",
                                                           .Image = m_HiZImage,
                                                           .MipLevels = mipCount,
                                                       });
        m_HiZSampleHandle = bindless.Register(m_HiZSampleView);

        // Per-mip reduction descriptor sets: set k binds mip k's source (the depth
        // target for k=0, hi-Z mip k-1 otherwise) and mip k's destination storage view.
        m_HiZReduceSets.clear();
        m_HiZReduceSets.reserve(mipCount);
        for (u32 level = 0; level < mipCount; level++)
        {
            Ref<DescriptorSet> set = DescriptorSet::Create(
                m_Context, {
                               .Name = fmt::format("SceneRenderer HiZ Reduce Set {}", level),
                               .Layout = m_HiZReduceSetLayout,
                           });
            const Ref<ImageView>& source = level == 0 ? m_DepthView : m_HiZMips[level - 1];
            set->Write(0, source);
            set->Write(1, m_HiZMips[level]);
            m_HiZReduceSets.push_back(std::move(set));
        }

        // The freshly created pyramid carries no last-frame depth; the next Execute must
        // skip occlusion rather than test against an undefined/stale chain.
        m_HiZHistoryReset = true;

        // The cull set samples the pyramid through binding 0, and the pyramid is recreated on
        // Resize/Configure — but the live set may still be referenced by an in-flight frame's
        // command buffer and its bindings are not update-after-bind, so bind the new view into
        // a fresh set rather than writing the old one in place (the replaced set retires through
        // the deferred-destruction path, and the cull pass reads the member at record time).
        // Skipped on the first CreateHiZ, before
        // CreateCullResources has made the set and the buffers it binds.
        if (m_CullSet)
        {
            m_CullSet = DescriptorSet::Create(m_Context, {
                                                             .Name = "SceneRenderer Cull Set",
                                                             .Layout = m_CullSetLayout,
                                                         });
            m_CullSet->Write(0, m_HiZSampleView);
            m_CullSet->Write(1, m_CullCandidateBuffer);
            m_CullSet->Write(2, m_IndirectBuffer);
            m_CullSet->Write(3, m_CullCountBuffer);
        }
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

        // The GPU cull path's buffers + pipeline build only where the device supports it.
        if (!m_Context.IsGpuDrivenCullingSupported())
        {
            return;
        }

        const u64 candidateRegion = static_cast<u64>(MaxCullCandidates) * sizeof(GpuCullCandidate);
        m_CullCandidateBuffer =
            Buffer::Create(m_Context, {
                                          .Name = "SceneRenderer Cull Candidates",
                                          .Size = candidateRegion * m_FramesInFlight,
                                          .Usage = BufferUsage::Storage,
                                          .HostMapped = true,
                                      });

        const u64 indirectRegion =
            static_cast<u64>(MaxCullCandidates) * sizeof(DrawIndexedIndirectCommand);
        m_IndirectBuffer =
            Buffer::Create(m_Context, {
                                          .Name = "SceneRenderer Indirect Commands",
                                          .Size = indirectRegion * m_FramesInFlight,
                                          .Usage = BufferUsage::Storage | BufferUsage::Indirect,
                                      });

        m_CullCountBuffer =
            Buffer::Create(m_Context, {
                                          .Name = "SceneRenderer Cull Count",
                                          .Size = static_cast<u64>(m_FramesInFlight) * sizeof(u32),
                                          .Usage = BufferUsage::Storage | BufferUsage::TransferSrc,
                                          .HostMapped = true,
                                      });

        const AssetResult<AssetHandle<Veng::Shader>> cullCs =
            m_Assets.LoadSync<Veng::Shader>(OcclusionCullCompId);
        VE_ASSERT(cullCs.has_value(), "SceneRenderer: occlusion-cull compute load failed: {}",
                  cullCs.error().Detail);

        m_CullSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Cull Set Layout",
                           .Bindings =
                               {
                                   {.Binding = 0,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 1,
                                    .Type = DescriptorType::StorageBuffer,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 2,
                                    .Type = DescriptorType::StorageBuffer,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 3,
                                    .Type = DescriptorType::StorageBuffer,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                               },
                       });
        m_CullLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Cull Layout",
                           .DescriptorSetLayouts = {m_CullSetLayout},
                           .PushConstantRanges = {PushConstantRange::Of<OcclusionCullPush>(
                               ShaderStage::Compute)},
                       });
        m_CullPipeline = ComputePipeline::Create(
            m_Context,
            {
                .Name = "SceneRenderer Cull Pipeline",
                .PipelineLayout = m_CullLayout,
                .ShaderStage = {.Stage = ShaderStage::Compute, .Module = cullCs->Get()->Module},
            });

        m_CullSet = DescriptorSet::Create(m_Context, {
                                                         .Name = "SceneRenderer Cull Set",
                                                         .Layout = m_CullSetLayout,
                                                     });
        m_CullSet->Write(0, m_HiZSampleView);
        m_CullSet->Write(1, m_CullCandidateBuffer);
        m_CullSet->Write(2, m_IndirectBuffer);
        m_CullSet->Write(3, m_CullCountBuffer);
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

    uvec2 SceneRenderer::SsrRenderExtent() const
    {
        if (m_Settings.SsrResolutionScale == SceneRendererSettings::SsrResolution::Half)
        {
            return glm::max(uvec2(1), m_Extent / 2u);
        }
        if (m_Settings.SsrResolutionScale == SceneRendererSettings::SsrResolution::Quarter)
        {
            return glm::max(uvec2(1), m_Extent / 4u);
        }
        return m_Extent;
    }

    void SceneRenderer::CreateRefraction()
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_RefractionSceneHandle);
        bindless.Release(m_RefractionDepthHandle);
        m_RefractionSceneHandle = {};
        m_RefractionDepthHandle = {};

        if (!m_Settings.Refraction)
        {
            m_RefractionSceneImage.reset();
            m_RefractionSceneView.reset();
            m_RefractionDepthImage.reset();
            m_RefractionDepthView.reset();
            return;
        }

        // The pre-translucent scene color and opaque depth the copy pass fills each frame;
        // translucent materials sample them through the view block's SceneColor handles.
        m_RefractionSceneImage =
            Image::Create(m_Context, {
                                         .Name = "SceneRenderer Refraction Scene",
                                         .Extent = {m_Extent.x, m_Extent.y, 1},
                                         .Format = HdrFormat,
                                         .Usage = HdrUsage,
                                     });
        m_RefractionSceneView =
            ImageView::Create(m_Context, {.Name = "SceneRenderer Refraction Scene View",
                                          .Image = m_RefractionSceneImage});
        m_RefractionSceneHandle = bindless.Register(m_RefractionSceneView);

        m_RefractionDepthImage =
            Image::Create(m_Context, {
                                         .Name = "SceneRenderer Refraction Depth",
                                         .Extent = {m_Extent.x, m_Extent.y, 1},
                                         .Format = Format::R32Sfloat,
                                         .Usage = HdrUsage,
                                     });
        m_RefractionDepthView =
            ImageView::Create(m_Context, {.Name = "SceneRenderer Refraction Depth View",
                                          .Image = m_RefractionDepthImage});
        m_RefractionDepthHandle = bindless.Register(m_RefractionDepthView);
    }

    void SceneRenderer::CreateSsr()
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_SsrSceneHandle);
        bindless.Release(m_SsrReflectionSampleHandle);
        bindless.Release(m_SsrReflectionSamplerHandle);
        bindless.Release(m_SsrHiZSampleHandle);
        m_SsrSceneHandle = {};
        m_SsrReflectionSampleHandle = {};
        m_SsrReflectionSamplerHandle = {};
        m_SsrHiZSampleHandle = {};

        // SSR targets exist only when SSR runs (the toggle or the Reflections debug arm).
        const bool ssrWanted = m_Settings.SSR || m_Settings.Mode == DebugView::Reflections;
        if (!ssrWanted)
        {
            m_SsrSceneImage.reset();
            m_SsrSceneView.reset();
            m_SsrReflectionImage.reset();
            m_SsrReflectionMips.clear();
            m_SsrReflectionSampleView.reset();
            m_SsrReflectionSampler.reset();
            m_SsrBlurSets.clear();
            m_SsrHiZImage.reset();
            m_SsrHiZMips.clear();
            m_SsrHiZSampleView.reset();
            m_SsrHiZReduceSets.clear();
            return;
        }

        // The lit scene color SSR reads: lighting/TAA writes here, the composite reflects into
        // it and writes the HDR target, so the bloom/tonemap tail is unchanged.
        m_SsrSceneImage = Image::Create(m_Context, {
                                                       .Name = "SceneRenderer SSR Scene",
                                                       .Extent = {m_Extent.x, m_Extent.y, 1},
                                                       .Format = HdrFormat,
                                                       .Usage = HdrUsage,
                                                   });
        m_SsrSceneView = ImageView::Create(
            m_Context, {.Name = "SceneRenderer SSR Scene View", .Image = m_SsrSceneImage});
        m_SsrSceneHandle = bindless.Register(m_SsrSceneView);

        // The trace, min-Z pyramid, and blur chain run at the SSR resolution (the scene color
        // above stays full-res: the trace samples it by reflected UV, the composite by logical UV).
        const uvec2 ssrExtent = SsrRenderExtent();

        // The reflection mip chain: mip 0 the trace writes, coarser mips the blur produces. The
        // chain stops BloomTileShift levels short of 1×1 (a rough reflection needs no 1-px mip).
        const u32 maxDim = std::max(ssrExtent.x, ssrExtent.y);
        const u32 mipCount =
            maxDim == 0 ? 1u : std::max(1u, std::bit_width(maxDim) - BloomTileShift);

        m_SsrReflectionImage =
            Image::Create(m_Context, {
                                         .Name = "SceneRenderer SSR Reflection",
                                         .Extent = {ssrExtent.x, ssrExtent.y, 1},
                                         .MipLevels = mipCount,
                                         .Format = HdrFormat,
                                         .Usage = ImageUsage::ColorAttachment |
                                                  ImageUsage::Storage | ImageUsage::Sampled,
                                     });

        m_SsrReflectionMips.clear();
        m_SsrReflectionMips.reserve(mipCount);
        for (u32 level = 0; level < mipCount; level++)
        {
            m_SsrReflectionMips.push_back(ImageView::Create(
                m_Context,
                {
                    .Name = fmt::format("SceneRenderer SSR Reflection Mip {} View", level),
                    .Image = m_SsrReflectionImage,
                    .BaseMipLevel = level,
                    .MipLevels = 1,
                }));
        }
        m_SsrReflectionSampleView =
            ImageView::Create(m_Context, {
                                             .Name = "SceneRenderer SSR Reflection Sample View",
                                             .Image = m_SsrReflectionImage,
                                             .MipLevels = mipCount,
                                         });
        m_SsrReflectionSampleHandle = bindless.Register(m_SsrReflectionSampleView);

        // Trilinear over the chain so the composite's roughness LOD blends between mips smoothly.
        m_SsrReflectionSampler =
            Sampler::Create(m_Context, {
                                           .Name = "SceneRenderer SSR Reflection Sampler",
                                           .MagFilter = Filter::Linear,
                                           .MinFilter = Filter::Linear,
                                           .MipmapMode = MipmapMode::Linear,
                                           .AddressModeU = AddressMode::ClampToEdge,
                                           .AddressModeV = AddressMode::ClampToEdge,
                                           .AddressModeW = AddressMode::ClampToEdge,
                                           .AnisotropyEnabled = false,
                                           .MaxLod = static_cast<f32>(mipCount),
                                       });
        m_SsrReflectionSamplerHandle = bindless.Register(m_SsrReflectionSampler);

        // Per-level blur sets (the bloom down/up set layout: sampled source + sampler + storage
        // dest). Set k reads mip k-1 and writes mip k; index 0 produces mip 1.
        m_SsrBlurSets.clear();
        if (mipCount > 1)
        {
            m_SsrBlurSets.reserve(mipCount - 1);
            for (u32 level = 1; level < mipCount; level++)
            {
                Ref<DescriptorSet> set = DescriptorSet::Create(
                    m_Context, {
                                   .Name = fmt::format("SceneRenderer SSR Blur Set {}", level),
                                   .Layout = m_Bloom->GetDownUpSetLayout(),
                               });
                set->Write(0, m_SsrReflectionMips[level - 1]);
                set->Write(1, m_SsrReflectionSampler);
                set->Write(2, m_SsrReflectionMips[level]);
                m_SsrBlurSets.push_back(std::move(set));
            }
        }

        // Min-Z depth pyramid (mirrors CreateHiZ, opposite reduction): a full mip chain the
        // trace marches through to skip empty space. Reduced from this frame's depth before the
        // trace; distinct from the occlusion-culling max-Z pyramid.
        const u32 hizMips = maxDim == 0 ? 1u : std::bit_width(maxDim);
        m_SsrHiZImage =
            Image::Create(m_Context, {
                                         .Name = "SceneRenderer SSR MinZ",
                                         .Extent = {ssrExtent.x, ssrExtent.y, 1},
                                         .MipLevels = hizMips,
                                         .Format = HiZFormat,
                                         .Usage = ImageUsage::Storage | ImageUsage::Sampled,
                                     });
        m_SsrHiZMips.clear();
        m_SsrHiZMips.reserve(hizMips);
        for (u32 level = 0; level < hizMips; level++)
        {
            m_SsrHiZMips.push_back(ImageView::Create(
                m_Context, {
                               .Name = fmt::format("SceneRenderer SSR MinZ Mip {} View", level),
                               .Image = m_SsrHiZImage,
                               .BaseMipLevel = level,
                               .MipLevels = 1,
                           }));
        }
        m_SsrHiZSampleView =
            ImageView::Create(m_Context, {
                                             .Name = "SceneRenderer SSR MinZ Sample View",
                                             .Image = m_SsrHiZImage,
                                             .MipLevels = hizMips,
                                         });
        m_SsrHiZSampleHandle = bindless.Register(m_SsrHiZSampleView);

        // Per-mip reduction sets (the hi-Z reduce set layout): set k binds mip k's source (the
        // depth target for k=0, min-Z mip k-1 otherwise) and mip k's destination storage view.
        m_SsrHiZReduceSets.clear();
        m_SsrHiZReduceSets.reserve(hizMips);
        for (u32 level = 0; level < hizMips; level++)
        {
            Ref<DescriptorSet> set = DescriptorSet::Create(
                m_Context, {
                               .Name = fmt::format("SceneRenderer SSR MinZ Reduce Set {}", level),
                               .Layout = m_HiZReduceSetLayout,
                           });
            const Ref<ImageView>& source = level == 0 ? m_DepthView : m_SsrHiZMips[level - 1];
            set->Write(0, source);
            set->Write(1, m_SsrHiZMips[level]);
            m_SsrHiZReduceSets.push_back(std::move(set));
        }
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
        const bool bakedSkyWanted = sceneComposited &&
                                    (m_ResolvedSkyKind == SkySourceKind::Material ||
                                     m_ResolvedSkyKind == SkySourceKind::Atmosphere) &&
                                    m_ResolvedSkyBaked;
        const bool cubeBacked =
            (sceneComposited && m_ResolvedSkyKind == SkySourceKind::Environment) || bakedSkyWanted;
        const bool skyboxWanted = cubeBacked;
        const bool atmosphereWanted = sceneComposited &&
                                      m_ResolvedSkyKind == SkySourceKind::Atmosphere &&
                                      !m_ResolvedSkyBaked;
        const bool skyMaterialWanted =
            sceneComposited && m_ResolvedSkyKind == SkySourceKind::Material && !m_ResolvedSkyBaked;
        const bool skylightWanted = cubeBacked && m_ResolvedSkyLighting == SkyLighting::SH;
        m_SkylightActive = skylightWanted;

        // The skybox pass samples the IBL radiance set for an environment sky, or the bake's
        // consumer set (same radiance binding) for a baked material/atmosphere sky.
        const Ref<DescriptorSet> skyboxSet = bakedSkyWanted ? m_SkyBake->GetSet() : m_Ibl->GetSet();

        // IBL lights the scene when the resolved sky is a cube-backed source on the IBL tier — an
        // environment (convolved from its equirect cube) or a baked material/atmosphere (convolved
        // from its bake cube). Either fills the IBL consumer set the lighting pass binds; a
        // display-only source (any other tier) shows its sky without lighting from it.
        const bool iblAllowed = cubeBacked && m_ResolvedSkyLighting == SkyLighting::IBL;

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
            m_SsrReflectionChainId = graph.ImportImageMips(
                "SceneRenderer SSR Reflection", static_cast<u32>(m_SsrReflectionMips.size()));
            m_SsrHiZChainId = graph.ImportImageMips("SceneRenderer SSR MinZ",
                                                    static_cast<u32>(m_SsrHiZMips.size()));
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
            taaActive ? m_Taa->GetLitHandle() : (ssrActive ? m_SsrSceneHandle : m_HdrHandle);

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
        m_IndirectId = ResourceId{};
        if (m_ActiveCull == SceneRendererSettings::CullMode::GPU)
        {
            m_IndirectId = graph.ImportBuffer("SceneRenderer Indirect Commands");
        }

        auto gbufferPass = CreateUnique<GBufferScenePass>(m_Context, m_Extent, &m_Internal->Plan,
                                                          m_ActiveCull, m_IndirectId);
        m_Passes.push_back(std::move(gbufferPass));

        // The entity-id picking pass: a depth-tested re-draw of the same survivors into the R32Uint
        // EntityId target through its own RenderingInfo (the shipping g-buffer pass above untouched).
        // Allocated and wired only when Settings.Picking is set, so the shipping path is unchanged.
        m_PickingActive = m_Settings.Picking && m_EntityIdImage != nullptr;
        m_EntityIdId = ResourceId{};
        m_PickingDepthId = ResourceId{};
        if (m_PickingActive)
        {
            m_EntityIdId = graph.Import("SceneRenderer EntityId");
            m_PickingDepthId = graph.Import("SceneRenderer Picking Depth");
            m_Passes.push_back(CreateUnique<PickingScenePass>(
                m_Context, m_Extent, &m_Internal->Plan, &m_PickingPipeline,
                &m_PickingSkinnedPipeline, m_EntityIdId, m_PickingDepthId));

            // The billboard id-write runs immediately after the mesh id pass — still in the
            // geometry-pass timeframe, while the EntityId target is bound — writing each pickable
            // billboard's owning entity id over a min-size proxy footprint, hardware-depth-discarded
            // against the mesh depth the picking pass just stored. Decorative billboards (PickId 0)
            // are untouched here; the DebugDrawScenePass still draws them after tonemap unchanged.
            m_Passes.push_back(CreateUnique<BillboardPickScenePass>(
                m_Context, m_Assets, &m_DebugDraw, GBuffer::DepthFormat,
                m_Context.GetMaxFramesInFlight(), m_Extent, m_EntityIdId, m_PickingDepthId));
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
                m_Shadows->GetPunctualRingStride(), m_Ibl->GetSet(), m_Ibl->GetPrefilterMipCount(),
                skylightWanted, iblAllowed));

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
                    m_Context, m_SkyPipeline, m_Atmosphere->GetSet(), lightingTargetId, depthId,
                    m_DepthHandle, m_SamplerHandle, m_Extent));
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
                m_Passes.push_back(CreateUnique<SceneColorCopyScenePass>(
                    m_Context, m_SceneColorCopyPipeline, lightingTargetId, lightingTargetHandle,
                    depthId, m_DepthHandle, m_RefractionSceneId, m_RefractionDepthId,
                    m_SamplerHandle, m_Extent));
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
                CreateUnique<FullscreenBlitScenePass>(m_Context, m_AlbedoBlitPipeline, m_Extent,
                                                      FullscreenBlitScenePass::Source::Albedo));
            break;
        case DebugView::Normal:
            m_Passes.push_back(
                CreateUnique<FullscreenBlitScenePass>(m_Context, m_NormalBlitPipeline, m_Extent,
                                                      FullscreenBlitScenePass::Source::Normal));
            break;
        case DebugView::Depth:
            m_Passes.push_back(CreateUnique<FullscreenBlitScenePass>(
                m_Context, m_DepthBlitPipeline, m_Extent, FullscreenBlitScenePass::Source::Depth));
            break;
        case DebugView::Occlusion:
            m_Passes.push_back(CreateUnique<OrmBlitScenePass>(m_Context, m_OrmBlitPipeline,
                                                              m_Extent, /*channel=*/0));
            break;
        case DebugView::Roughness:
            m_Passes.push_back(CreateUnique<OrmBlitScenePass>(m_Context, m_OrmBlitPipeline,
                                                              m_Extent, /*channel=*/1));
            break;
        case DebugView::Metallic:
            m_Passes.push_back(CreateUnique<OrmBlitScenePass>(m_Context, m_OrmBlitPipeline,
                                                              m_Extent, /*channel=*/2));
            break;
        case DebugView::AO:
            m_Passes.push_back(CreateUnique<FullscreenBlitScenePass>(
                m_Context, m_AoBlitPipeline, m_Extent, FullscreenBlitScenePass::Source::Ao));
            break;
        case DebugView::Shadows:
            // Reads the cascade atlas through the dedicated set (raw depth), not bindless.
            m_Passes.push_back(CreateUnique<ShadowBlitScenePass>(
                m_Context, m_ShadowBlitPipeline, m_Extent, m_Shadows->GetBlitSet(),
                ShadowBlitScenePass::Source::Directional));
            break;
        case DebugView::PunctualShadows:
            // Reads the punctual atlas through the dedicated set; binding 0 is
            // rewritten below after the pass set is chosen.
            m_Passes.push_back(CreateUnique<ShadowBlitScenePass>(
                m_Context, m_ShadowBlitPipeline, m_Extent, m_Shadows->GetBlitSet(),
                ShadowBlitScenePass::Source::Punctual));
            break;
        case DebugView::Cascades:
            // Tints fragments by cascade selection and writes the output directly (no tonemap tail).
            m_Passes.push_back(CreateUnique<DeferredLightingScenePass>(
                m_Context, m_CascadeDebugPipeline, m_Extent, /*useSsao=*/false, m_Shadows->GetSet(),
                m_Shadows->GetConstantsRingStride(), m_Shadows->GetPunctualRingStride(),
                m_Ibl->GetSet(), m_Ibl->GetPrefilterMipCount(), skylightWanted, iblAllowed,
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
                m_Shadows->GetPunctualRingStride(), m_Ibl->GetSet(), m_Ibl->GetPrefilterMipCount(),
                skylightWanted, iblAllowed));
            if (skyboxWanted)
            {
                m_Passes.push_back(CreateUnique<SkyboxScenePass>(
                    m_Context, m_SkyboxPipeline, skyboxSet, lightingTargetId, depthId,
                    m_DepthHandle, m_SamplerHandle, m_Extent, bakedSkyWanted));
            }
            if (atmosphereWanted)
            {
                m_Passes.push_back(CreateUnique<SkyScenePass>(
                    m_Context, m_SkyPipeline, m_Atmosphere->GetSet(), lightingTargetId, depthId,
                    m_DepthHandle, m_SamplerHandle, m_Extent));
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
                m_Passes.push_back(CreateUnique<SceneColorCopyScenePass>(
                    m_Context, m_SceneColorCopyPipeline, lightingTargetId, lightingTargetHandle,
                    depthId, m_DepthHandle, m_RefractionSceneId, m_RefractionDepthId,
                    m_SamplerHandle, m_Extent));
            }
            m_Passes.push_back(CreateUnique<TranslucentScenePass>(
                m_Context, m_Extent, &m_Internal->TranslucentPlan, lightingTargetId, depthId,
                m_RefractionSceneId, m_RefractionDepthId, HdrFormat));
            m_Passes.push_back(CreateUnique<FullscreenBlitScenePass>(
                m_Context, m_AlbedoBlitPipeline, m_Extent, FullscreenBlitScenePass::Source::Bloom));
            break;
        case DebugView::MotionVectors:
            // The g-buffer pass writes the velocity target (G3); this blit colorizes it as an
            // optical-flow field.
            m_Passes.push_back(CreateUnique<FullscreenBlitScenePass>(
                m_Context, m_MotionBlitPipeline, m_Extent,
                FullscreenBlitScenePass::Source::MotionVectors));
            break;
        case DebugView::Reflections:
            // Lighting writes the scene-color intermediate the force-wired SSR trace reflects
            // (DeclareSsr, before this blit); the blit shows the raw reflection target.
            m_Passes.push_back(CreateUnique<DeferredLightingScenePass>(
                m_Context, m_LightingPipeline, m_Extent, /*useSsao=*/false, m_Shadows->GetSet(),
                m_Shadows->GetConstantsRingStride(), m_Shadows->GetPunctualRingStride(),
                m_Ibl->GetSet(), m_Ibl->GetPrefilterMipCount(), skylightWanted, iblAllowed));
            m_Passes.push_back(CreateUnique<FullscreenBlitScenePass>(
                m_Context, m_AlbedoBlitPipeline, m_Extent,
                FullscreenBlitScenePass::Source::Reflections));
            break;
        case DebugView::Emissive:
            // The g-buffer pass writes the emissive channel (G4); this blit shows the authored
            // emissive contribution alone, the channel inspectable like every other g-buffer arm.
            m_Passes.push_back(
                CreateUnique<FullscreenBlitScenePass>(m_Context, m_AlbedoBlitPipeline, m_Extent,
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
            .SsrReflectionHandle = m_SsrReflectionSampleHandle,
            .SamplerHandle = m_SamplerHandle,
            .LtcMatHandle = m_LtcMatHandle,
            .LtcMagHandle = m_LtcMagHandle,
            .ShadowMap = shadowId,
            .ShadowView = shadowAtlasView,
            .PunctualShadowMap = punctualShadowId,
            .PunctualShadowView = m_Shadows->GetPunctualView(),
            .Output = m_OutputId,
        };

        // Import the hi-Z chain once: the GPU cull samples last frame's pyramid (declared
        // .Sample below for the graph-derived transition into ShaderReadOnly before the cull)
        // and the reduction at the tail writes this frame's pyramid into the same slots.
        m_HiZChainId =
            graph.ImportImageMips("SceneRenderer HiZ", static_cast<u32>(m_HiZMips.size()));

        // The GPU cull compute pass must precede the geometry pass: it writes the
        // indirect commands the geometry pass reads. Declared before the pass loop so it
        // is earlier in the graph's declaration (execution) order than the g-buffer pass.
        if (m_ActiveCull == SceneRendererSettings::CullMode::GPU)
        {
            DeclareCullPass(graph);
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
                    DeclareSsr(graph);
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
        DeclareHiZReduction(graph);

        m_Internal->Graph = graph.Compile();
    }

    void SceneRenderer::ResolveSky(SceneView& view)
    {
        // Start from the no-sky fallback; the resolved source overrides what it drives.
        view.Environment = {};
        view.EnvironmentIntensity = 1.0f;
        view.AtmosphereEnabled = false;
        view.AtmosphereIntensity = 1.0f;
        view.Atmosphere = Atmosphere{};
        view.SkyMaterial = {};
        view.SkylightIntensity = 1.0f;

        // The toward-sun direction is derived from the scene's first directional light (a sun
        // overhead travels down), so the sky and the direct lighting share one sun. No directional
        // light leaves the default world-up sun.
        view.SunDirection = vec3(0.0f, 1.0f, 0.0f);
        for (auto [entity, light] : view.World.View<Light>())
        {
            if (light.Type == LightType::Directional)
            {
                const f32 length = glm::length(light.Direction);
                if (length > 1e-4f)
                {
                    view.SunDirection = -light.Direction / length;
                }
                break;
            }
        }

        // Resolve the scene's one Sky component; warn once if several exist (the first walked wins).
        const Sky* sky = view.World.TryGetFirst<Sky>();
        u32 skyCount = 0;
        for ([[maybe_unused]] auto [entity, component] : view.World.View<Sky>())
        {
            ++skyCount;
        }
        if (skyCount > 1 && !m_MultipleSkyWarned)
        {
            Log::Warn(
                "SceneRenderer: {} Sky components in the scene; resolving the first, ignoring "
                "the rest.",
                skyCount);
            m_MultipleSkyWarned = true;
        }

        SkySourceKind kind = SkySourceKind::None;
        SkyLighting lighting = SkyLighting::None;
        bool baked = false;

        if (sky != nullptr && sky->Source.HasValue())
        {
            lighting = sky->Lighting;
            const TypeId active = sky->Source.ActiveType();
            const void* source = sky->Source.ActivePtr();
            if (active == TypeIdOf<EnvironmentSky>())
            {
                const auto* environment = static_cast<const EnvironmentSky*>(source);
                view.Environment = environment->Map;
                view.EnvironmentIntensity = sky->Intensity;
                kind = SkySourceKind::Environment;
            }
            else if (active == TypeIdOf<AtmosphereSky>())
            {
                const auto* atmosphere = static_cast<const AtmosphereSky*>(source);
                view.Atmosphere = atmosphere->Params;
                view.AtmosphereEnabled = true;
                view.AtmosphereIntensity = sky->Intensity;
                view.SkylightIntensity = sky->Intensity;
                kind = SkySourceKind::Atmosphere;
                // Baked renders the atmosphere into a radiance cube the skybox path samples; direct
                // runs it per pixel every frame. The two render the same sky; the author picks per
                // the sky's dynamics and the renderer honors it (no silent switch).
                baked = atmosphere->Mode == SkyMode::Baked;
            }
            else if (active == TypeIdOf<MaterialSky>())
            {
                const auto* material = static_cast<const MaterialSky*>(source);
                view.SkyMaterial = material->Material;
                kind = SkySourceKind::Material;
                // Baked runs the material into a radiance cube the skybox path samples; direct runs
                // it per pixel every frame. The two render the same sky; the author picks per the
                // sky's dynamics and the renderer honors it (no silent switch).
                baked = material->Mode == SkyMode::Baked;
                view.EnvironmentIntensity = sky->Intensity;
            }
        }

        // The source × mode × tier resolution table, now in its final unified shape: every source is
        // a radiance-cube producer, so a lighting tier is active exactly when the source fills a
        // cube. An environment always does (its own radiance cube); a material or atmosphere does in
        // Baked mode (the bake cube), and does not in Direct mode (it composites per pixel, no cube
        // to light from). A direct source with a lighting tier therefore degrades to background-only
        // — bake to light — logged once. None is always display-only.
        const bool cubeBacked =
            kind == SkySourceKind::Environment ||
            ((kind == SkySourceKind::Material || kind == SkySourceKind::Atmosphere) && baked);
        const bool tierActive = lighting == SkyLighting::None || cubeBacked;
        if (!tierActive && !m_UnsupportedTierWarned)
        {
            Log::Warn("SceneRenderer: a direct sky cannot light the scene; displaying the sky "
                      "without lighting it — bake the sky (SkyMode::Baked) to light.");
            m_UnsupportedTierWarned = true;
        }

        // A degraded tier resolves to no lighting (display-only); the source still shows. The
        // lighting pass's iblAllowed/skylight flags — set from the resolved tier in Rebuild — gate
        // whether the scene is actually lit.
        const SkyLighting resolvedLighting = tierActive ? lighting : SkyLighting::None;

        // Drive the internal recompile on a resolved source-kind, tier, or bake-mode change — the
        // frame boundary the pass set flips at, reusing the imported output (identity preserved). A
        // direct↔baked flip is a resolved-source change: the same recompile a kind change drives.
        if (kind != m_ResolvedSkyKind || resolvedLighting != m_ResolvedSkyLighting ||
            baked != m_ResolvedSkyBaked)
        {
            m_ResolvedSkyKind = kind;
            m_ResolvedSkyLighting = resolvedLighting;
            m_ResolvedSkyBaked = baked;
            Rebuild();
        }
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

    void SceneRenderer::DeclareHiZReduction(RenderGraph& graph)
    {
        const u32 mipCount = static_cast<u32>(m_HiZMips.size());

        // One compute dispatch per mip. Dispatch k reads mip k's source and writes mip
        // k; the per-mip graph surface derives the read-after-write barrier between
        // dispatch k's write of mip k and dispatch k+1's read of it. Mip 0's source is
        // the depth target (declared .Sample, reusing the depth import so the barrier
        // chains off the lighting pass's read); a source mip n-1 is declared .StorageRead.
        for (u32 level = 0; level < mipCount; level++)
        {
            // Mip extents (image extent >> level, floored at 1).
            const u32 dstW = std::max(m_Extent.x >> level, 1u);
            const u32 dstH = std::max(m_Extent.y >> level, 1u);
            const u32 srcW = level == 0 ? m_Extent.x : std::max(m_Extent.x >> (level - 1), 1u);
            const u32 srcH = level == 0 ? m_Extent.y : std::max(m_Extent.y >> (level - 1), 1u);

            // The source (depth target or prior mip) binds as a sampled image, so it
            // must be in ShaderReadOnly — declared .Sample, not .StorageRead. The
            // destination is a storage write (General). A prior mip therefore goes
            // General (its write) → ShaderReadOnly (its read as the next source), a
            // graph-derived per-mip transition.
            RenderGraph::PassBuilder builder =
                graph.AddComputePass(fmt::format("HiZ Reduce Mip {}", level));
            if (level == 0)
            {
                builder.Sample(m_DepthId);
            }
            else
            {
                builder.Sample(m_HiZChainId.Level(level - 1));
            }
            builder.StorageWrite(m_HiZChainId.Level(level));

            const Ref<ComputePipeline> pipeline = m_HiZReducePipeline;
            const Ref<DescriptorSet> set = m_HiZReduceSets[level];
            const HiZReducePush push{
                .DestExtent = {dstW, dstH},
                .SourceExtent = {srcW, srcH},
            };
            builder.Execute(
                [pipeline, set, push](PassContext& inner)
                {
                    CommandBuffer& cmd = inner.Cmd();
                    cmd.BindPipeline(pipeline);
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {set},
                        .FirstSet = 1, // set 0 is reserved for the bindless registry
                        .PipelineBindPoint = PipelineBindPoint::Compute,
                    });
                    cmd.PushConstants(push);
                    cmd.Dispatch((push.DestExtent.x + 7) / 8, (push.DestExtent.y + 7) / 8, 1);
                });
        }
    }

    void SceneRenderer::DeclareSsr(RenderGraph& graph)
    {
        const u32 mipCount = static_cast<u32>(m_SsrReflectionMips.size());
        const u32 hizMips = static_cast<u32>(m_SsrHiZMips.size());
        const uvec2 ssrExtent = SsrRenderExtent();

        // Min-Z reduction: build this frame's closest-surface pyramid from the depth target
        // before the trace. One dispatch per mip; mip 0 ingests the full-res depth target into
        // the SSR-resolution pyramid (a downsample when SSR runs below full res, a 1:1 copy at
        // Full), deeper mips halve the prior. The per-mip graph surface derives the
        // read-after-write barriers; the trace then reads the whole chain. Mirrors
        // DeclareHiZReduction with the min-Z pipeline.
        for (u32 level = 0; level < hizMips; level++)
        {
            const u32 dstW = std::max(ssrExtent.x >> level, 1u);
            const u32 dstH = std::max(ssrExtent.y >> level, 1u);
            const u32 srcW = level == 0 ? m_Extent.x : std::max(ssrExtent.x >> (level - 1), 1u);
            const u32 srcH = level == 0 ? m_Extent.y : std::max(ssrExtent.y >> (level - 1), 1u);

            RenderGraph::PassBuilder builder =
                graph.AddComputePass(fmt::format("SSR MinZ Reduce Mip {}", level));
            if (level == 0)
            {
                builder.Sample(m_DepthId);
            }
            else
            {
                builder.Sample(m_SsrHiZChainId.Level(level - 1));
            }
            builder.StorageWrite(m_SsrHiZChainId.Level(level));

            const Ref<ComputePipeline> pipeline = m_SsrHiZReducePipeline;
            const Ref<DescriptorSet> set = m_SsrHiZReduceSets[level];
            const HiZReducePush push{
                .DestExtent = {dstW, dstH},
                .SourceExtent = {srcW, srcH},
            };
            builder.Execute(
                [pipeline, set, push](PassContext& inner)
                {
                    CommandBuffer& cmd = inner.Cmd();
                    cmd.BindPipeline(pipeline);
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {set},
                        .FirstSet = 1, // set 0 is reserved for the bindless registry
                        .PipelineBindPoint = PipelineBindPoint::Compute,
                    });
                    cmd.PushConstants(push);
                    cmd.Dispatch((push.DestExtent.x + 7) / 8, (push.DestExtent.y + 7) / 8, 1);
                });
        }

        // Trace: reflect the view vector about the g-buffer normal, march the depth buffer, and
        // write the reflected scene radiance + a confidence mask into the reflection chain's mip 0.
        {
            const TextureHandle sceneHandle = m_SsrSceneHandle;
            const TextureHandle depthHandle = m_DepthHandle;
            const TextureHandle normalHandle = m_NormalHandle;
            const TextureHandle ormHandle = m_OrmHandle;
            const TextureHandle hizHandle = m_SsrHiZSampleHandle;
            const SamplerHandle samplerHandle = m_SamplerHandle;

            RenderGraph::PassBuilder traceBuilder = graph.AddPass("SSR Trace");
            traceBuilder
                .Color({
                    .Resource = m_SsrReflectionChainId.Level(0),
                    .Load = LoadOp::Clear,
                    .Store = StoreOp::Store,
                    .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
                })
                .Sample(m_SsrSceneId)
                .Sample(m_NormalId)
                .Sample(m_OrmId)
                .Sample(m_DepthId);
            // The trace Loads the whole min-Z chain by bindless handle; declaring each mip
            // sampled drives the graph-derived General → ShaderReadOnly transition after the
            // reduction wrote it.
            for (u32 level = 0; level < hizMips; level++)
            {
                traceBuilder.Sample(m_SsrHiZChainId.Level(level));
            }
            traceBuilder.Execute(
                [this, sceneHandle, depthHandle, normalHandle, ormHandle, hizHandle, hizMips,
                 samplerHandle, ssrExtent](PassContext& inner)
                {
                    CommandBuffer& cmd = inner.Cmd();
                    const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
                    const auto* viewPtr = static_cast<const SceneView*>(inner.UserData());
                    VE_ASSERT(viewPtr != nullptr, "SSR trace pass: null SceneView");
                    const SceneView& view = *viewPtr;
                    // The trace writes the SSR-resolution reflection target, so it marches and
                    // steps at the SSR extent; the g-buffer it reads stays full-res, sampled by
                    // logical UV. SSR disables the dynamic-resolution sub-rect, so RenderExtent
                    // is the full extent the SSR extent derives from.
                    const uvec2 renderExtent = ssrExtent;

                    cmd.BindPipeline(m_SsrTracePipeline);
                    cmd.SetViewport({0, 0}, renderExtent);
                    cmd.SetScissor({0, 0}, renderExtent);
                    registry.Bind(cmd);
                    cmd.PushConstants(SsrTracePush{
                        .SceneColorTexture = sceneHandle.Index,
                        .DepthTexture = depthHandle.Index,
                        .NormalTexture = normalHandle.Index,
                        .OrmTexture = ormHandle.Index,
                        .HiZTexture = hizHandle.Index,
                        .Sampler = samplerHandle.Index,
                        .ViewConstantsIndex = registry.GetCurrentViewConstantsIndex(),
                        .HiZLevels = hizMips,
                        .Extent = renderExtent,
                        .MaxDistance = view.SsrMaxDistance,
                        .Thickness = view.SsrThickness,
                        .MaxRoughness = view.SsrMaxRoughness,
                        .MaxSteps = 256,
                    });
                    cmd.DrawFullscreenTriangle();
                });
        }

        // Blur down-sweep: build the roughness mip chain (mip k averages mip k-1). The per-mip
        // graph surface derives the read-after-write barrier between writing mip k and reading it.
        const uvec2 allocExtent = ssrExtent;
        for (u32 level = 1; level < mipCount; level++)
        {
            const u32 dstW = std::max(allocExtent.x >> level, 1u);
            const u32 dstH = std::max(allocExtent.y >> level, 1u);
            const Ref<ComputePipeline> pipeline = m_SsrBlurPipeline;
            const Ref<DescriptorSet> set = m_SsrBlurSets[level - 1];

            graph.AddComputePass(fmt::format("SSR Blur Mip {}", level))
                .Sample(m_SsrReflectionChainId.Level(level - 1))
                .StorageWrite(m_SsrReflectionChainId.Level(level))
                .Execute(
                    [pipeline, set, dstW, dstH](PassContext& inner)
                    {
                        CommandBuffer& cmd = inner.Cmd();
                        cmd.BindPipeline(pipeline);
                        cmd.BindDescriptorSets(DescriptorSetBindInfo{
                            .Sets = {set},
                            .FirstSet = 1, // set 0 is reserved for the bindless registry
                            .PipelineBindPoint = PipelineBindPoint::Compute,
                        });
                        cmd.PushConstants(SsrBlurPush{.DestExtent = {dstW, dstH}});
                        cmd.Dispatch((dstW + 7) / 8, (dstH + 7) / 8, 1);
                    });
        }

        // Composite: fold the (roughness-LOD) reflection into the scene color and write the HDR
        // target the bloom/tonemap tail reads.
        {
            const TextureHandle sceneHandle = m_SsrSceneHandle;
            const TextureHandle reflectionHandle = m_SsrReflectionSampleHandle;
            const TextureHandle albedoHandle = m_AlbedoHandle;
            const TextureHandle normalHandle = m_NormalHandle;
            const TextureHandle ormHandle = m_OrmHandle;
            const TextureHandle depthHandle = m_DepthHandle;
            const SamplerHandle samplerHandle = m_SamplerHandle;
            const SamplerHandle reflSamplerHandle = m_SsrReflectionSamplerHandle;

            RenderGraph::PassBuilder builder = graph.AddPass("SSR Composite");
            builder
                .Color({
                    .Resource = m_HdrId,
                    .Load = LoadOp::Clear,
                    .Store = StoreOp::Store,
                    .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
                })
                .Sample(m_SsrSceneId)
                .Sample(m_NormalId)
                .Sample(m_OrmId)
                .Sample(m_DepthId);
            // Every reflection mip the composite's LOD sampling may read must be ShaderReadOnly.
            for (u32 level = 0; level < mipCount; level++)
            {
                builder.Sample(m_SsrReflectionChainId.Level(level));
            }

            builder.Execute(
                [this, sceneHandle, reflectionHandle, albedoHandle, normalHandle, ormHandle,
                 depthHandle, samplerHandle, reflSamplerHandle, mipCount](PassContext& inner)
                {
                    CommandBuffer& cmd = inner.Cmd();
                    const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
                    const auto* viewPtr = static_cast<const SceneView*>(inner.UserData());
                    VE_ASSERT(viewPtr != nullptr, "SSR composite pass: null SceneView");
                    const SceneView& view = *viewPtr;
                    const uvec2 renderExtent = view.RenderExtent;

                    cmd.BindPipeline(m_SsrCompositePipeline);
                    cmd.SetViewport({0, 0}, renderExtent);
                    cmd.SetScissor({0, 0}, renderExtent);
                    registry.Bind(cmd);
                    cmd.PushConstants(SsrCompositePush{
                        .SceneColorTexture = sceneHandle.Index,
                        .ReflectionTexture = reflectionHandle.Index,
                        .AlbedoTexture = albedoHandle.Index,
                        .NormalTexture = normalHandle.Index,
                        .OrmTexture = ormHandle.Index,
                        .DepthTexture = depthHandle.Index,
                        .Sampler = samplerHandle.Index,
                        .ReflectionSampler = reflSamplerHandle.Index,
                        .ViewConstantsIndex = registry.GetCurrentViewConstantsIndex(),
                        .Intensity = view.SsrIntensity,
                        .MipCount = mipCount,
                    });
                    cmd.DrawFullscreenTriangle();
                });
        }
    }

    void SceneRenderer::ResolveActiveCullMode()
    {
        const bool gpuRequested = m_Settings.Cull == SceneRendererSettings::CullMode::GPU;
        const bool gpuSupported = m_Context.IsGpuDrivenCullingSupported();

        // The GPU path is an optimization, not a correctness requirement: a device lacking
        // multiDrawIndirect / drawIndirectFirstInstance silently runs the CPU path, which
        // renders the same image. The fallback is logged once so it is observable, and
        // GetActiveCullMode() reports the real mode for the debug UI and tests.
        if (gpuRequested && !gpuSupported && !m_GpuCullWarned)
        {
            Log::Warn("SceneRenderer: CullMode::GPU requested but the device lacks "
                      "multiDrawIndirect / drawIndirectFirstInstance; using CullMode::CPU.");
            m_GpuCullWarned = true;
        }

        m_ActiveCull = (gpuRequested && gpuSupported) ? SceneRendererSettings::CullMode::GPU
                                                      : SceneRendererSettings::CullMode::CPU;
    }

    void SceneRenderer::DeclareCullPass(RenderGraph& graph)
    {
        // StorageBufferWrite on the indirect import drives the graph-derived
        // StorageBufferWrite → IndirectRead barrier feeding the geometry pass.
        RenderGraph::PassBuilder builder = graph.AddComputePass("Scene GPU Cull");
        builder.StorageBufferWrite(m_IndirectId);

        // The cull samples last frame's hi-Z pyramid (through m_CullSet, off the graph's
        // descriptor binding). Declaring .Sample on each mip drives the graph-derived
        // transition into ShaderReadOnly before the cull — the pyramid is a cross-frame
        // renderer-owned resource the graph would otherwise leave in its prior layout.
        for (u32 level = 0; level < m_HiZMips.size(); ++level)
        {
            builder.Sample(m_HiZChainId.Level(level));
        }

        builder.Execute(
            [this](PassContext& inner)
            {
                const GBufferDrawPlan& plan = m_Internal->Plan;
                if (plan.Cull != SceneRendererSettings::CullMode::GPU || plan.Slots.empty())
                {
                    return;
                }

                CommandBuffer& cmd = inner.Cmd();
                cmd.BindPipeline(m_CullPipeline);
                cmd.BindDescriptorSets(DescriptorSetBindInfo{
                    .Sets = {m_CullSet},
                    .FirstSet = 1, // set 0 is reserved for the bindless registry
                    .PipelineBindPoint = PipelineBindPoint::Compute,
                });
                cmd.PushConstants(OcclusionCullPush{
                    .PrevViewProj = m_CullPrevViewProj,
                    .HiZBaseExtent = m_CullHiZExtent,
                    .CandidateCount = m_CullCandidateCount,
                    .HistoryValid = m_CullHistoryValid,
                    // Small bias absorbing reduction quantization; a tie draws.
                    .DepthBias = 0.001f,
                    .FrameBase = m_CullFrameBase,
                    .CountIndex = m_CullCountIndex,
                });
                cmd.Dispatch((m_CullCandidateCount + 63) / 64, 1, 1);
            });
    }

    void SceneRenderer::PrepareDraws(const SceneView& view, const u32 viewConstantsIndex)
    {
        GBufferDrawPlan& plan = m_Internal->Plan;
        plan.Cull = m_ActiveCull;
        plan.DrawDataSet = m_DrawDataSet;
        plan.CandidateIdBuffer = m_CandidateIdBuffer;
        plan.IndirectBuffer = m_IndirectBuffer;
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
        GpuCullCandidate* cullData =
            m_CullCandidateBuffer
                ? static_cast<GpuCullCandidate*>(m_CullCandidateBuffer->GetMappedData()) +
                      static_cast<usize>(frameBase)
                : nullptr;

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

        // The GPU cull dispatch reads the candidate region this frame; zero its survivor
        // count so the next-frame readback reflects only this dispatch. The push members the
        // cull pass reads are set here (per-frame), not in the recompile-time declaration.
        if (m_ActiveCull == SceneRendererSettings::CullMode::GPU)
        {
            const u32 count = static_cast<u32>(plan.Slots.size());
            m_CullCandidateCount = count;
            m_CullFrameBase = frameBase;
            m_CullCountIndex = frameIndex;
            m_CullPrevViewProj = m_PreviousViewProj;
            m_CullHiZExtent = m_Extent;
            m_CullHistoryValid = (m_Settings.Occlusion && m_HiZHistoryValid) ? 1u : 0u;

            auto* counts = static_cast<u32*>(m_CullCountBuffer->GetMappedData());
            counts[frameIndex] = 0;

            // Record the region the readback reads one frame late.
            m_GpuCandidateCount = count;
            m_GpuReadbackRegion = frameIndex;
            m_GpuReadbackValid = true;
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
        CreateSsr();
        CreateRefraction();
        // The HDR target moved; rebind the metering source and re-snap the adaptation so the
        // resized frame is not mis-exposed off a stale ring value.
        m_AutoExposure->RebindHdr(m_HdrView);
        m_AutoExposure->RequestReset();
        m_Shadows->Reconfigure(m_Settings);
        CreatePicking();
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
        ResolveActiveCullMode();
        m_Taa->Resize(m_Extent, m_Settings.TAA);
        // The bloom pyramid is extent-driven (unchanged here); only the kernel choice may change.
        m_Bloom->Reconfigure(m_Settings.Kernel);
        CreateSsr();
        CreateRefraction();
        m_Shadows->Reconfigure(m_Settings);
        CreatePicking();
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
        // Scale applies only on the Final path with the sub-rect-aware battery set: a debug view, the
        // TAA resolve, the GPU hi-Z occlusion test, and the Dual-Kawase bloom kernel do not carry the
        // sub-rect sampling yet, so each forces full resolution (correct, just no scaling).
        const bool drsSupported =
            m_Settings.Mode == DebugView::Final && !m_Settings.TAA && !m_Settings.SSR &&
            !(m_ActiveCull == SceneRendererSettings::CullMode::GPU && m_Settings.Occlusion) &&
            !(m_Settings.Bloom && m_Settings.Kernel == BloomKernel::Kawase);
        const f32 renderScale = drsSupported ? view.RenderScale : 1.0f;
        const uvec2 validExtent =
            glm::clamp(uvec2(glm::round(vec2(m_Extent) * renderScale)), uvec2(1), m_Extent);
        const vec2 renderScaleUV = vec2(validExtent) / vec2(m_Extent);
        // Half-texel inset so a bilinear tap at the valid edge never reads past it.
        const vec2 maxValidUV = (vec2(validExtent) - 0.5f) / vec2(m_Extent);

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
        // at a higher frame rate. The broadphase tree stays built from the current-tick transforms
        // (its cull is conservative, so a sub-tick offset never drops a visible submesh); only the
        // drawn worlds interpolate. A static scene reports no motion history and skips the copy, so
        // its draw is byte-identical to the un-interpolated path.
        if (view.Alpha != 0.0f && view.World.HasTransformInterpolation())
        {
            const std::span<const VisibleMesh> candidates = m_Broadphase.GetCandidates();
            m_InterpolatedCandidates.assign(candidates.begin(), candidates.end());
            for (VisibleMesh& candidate : m_InterpolatedCandidates)
            {
                candidate.World =
                    view.World.GetInterpolatedWorldTransform(candidate.Owner, view.Alpha);
                candidate.WorldBounds = candidate.Mesh->GetBounds().Transformed(candidate.World);
            }
            resolvedView.Visible = m_InterpolatedCandidates;
        }

        // Resolve the scene's one Sky component into this frame's sky fields — the lights model,
        // the renderer reading the component off the scene the way it reads the lights. A resolved
        // source-kind or lighting-tier change recompiles the pass set at this frame boundary,
        // reusing the imported output so GetOutput() stays valid (only Resize/Configure recreate it).
        ResolveSky(resolvedView);

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

        BindlessRegistry& registry = m_Context.GetBindlessRegistry();

        // Procedural atmosphere: transition the LUTs to a sampled layout once, then regenerate them
        // only when this frame's Atmosphere differs from the last generated set — the once-per-change
        // contract (the sun direction is a runtime push, not a precompute input). Gated on the
        // resolved sky being an atmosphere so the cost is absent on the shipping path. Recorded here,
        // before the atmosphere bake below and before the graph the direct pass samples them
        // through, so the LUTs are resident for either display mode. A baked atmosphere generates
        // them through a self-contained immediate submit, so they are device-resident before the
        // bake's own immediate-submit readback samples them (the frame command buffer has not been
        // submitted at that point); a direct atmosphere records into the frame command buffer, which
        // the direct sky pass samples through in-order later this frame.
        m_AtmosphereRegeneratedLastFrame = false;
        if (m_ResolvedSkyKind == SkySourceKind::Atmosphere)
        {
            const bool regenerate = !m_AtmosphereGenerated ||
                                    !AtmosphereEquals(resolvedView.Atmosphere, m_LastAtmosphere);
            if (m_ResolvedSkyBaked)
            {
                m_Context.ImmediateCommands(
                    [&](CommandBuffer& lutCmd)
                    {
                        m_Atmosphere->EnsureInitialized(lutCmd);
                        if (regenerate)
                        {
                            m_Atmosphere->Generate(lutCmd, resolvedView.Atmosphere);
                        }
                    });
            }
            else
            {
                m_Atmosphere->EnsureInitialized(cmd);
                if (regenerate)
                {
                    m_Atmosphere->Generate(cmd, resolvedView.Atmosphere);
                }
            }
            if (regenerate)
            {
                m_LastAtmosphere = resolvedView.Atmosphere;
                m_AtmosphereGenerated = true;
                m_AtmosphereRegeneratedLastFrame = true;
            }
        }

        // Bake a baked-mode sky into its radiance cube on the sky dirty signal, so a static sky
        // bakes once and the skybox pass samples the cube this frame. Every baked source — an
        // authored material or the procedural atmosphere — fills the same cube; display and any
        // ambient tier then read that one cube, so they agree by construction. Recorded into cmd
        // before the graph the skybox pass samples the cube through, and before the scene claims its
        // own view slot below — the bake writes six face view-constants regions into distinct view
        // slots, so it must run ahead of the frame's own BeginView. A direct or no-sky source bakes
        // nothing.
        const bool bakedMaterial = m_ResolvedSkyKind == SkySourceKind::Material &&
                                   m_ResolvedSkyBaked && resolvedView.SkyMaterial.IsLoaded();
        const bool bakedAtmosphere =
            m_ResolvedSkyKind == SkySourceKind::Atmosphere && m_ResolvedSkyBaked;
        if (bakedMaterial || bakedAtmosphere)
        {
            // The dirty signal: for a material, its resolved instance changing; for the atmosphere,
            // its params or the sun direction changing (both feed the baked sky radiance + disc).
            const MaterialInstance* material =
                bakedMaterial ? resolvedView.SkyMaterial.Get() : nullptr;
            bool bakeDirty = false;
            if (bakedMaterial)
            {
                // The instance may be reused in place — its params/star buffer rewritten while the
                // pointer stays the same — so a revision change re-bakes as a swap does.
                bakeDirty = material != m_LastBakedSkyMaterial ||
                            material->GetRevision() != m_LastBakedSkyMaterialRevision;
            }
            else
            {
                bakeDirty = !m_BakedAtmosphereValid ||
                            !AtmosphereEquals(resolvedView.Atmosphere, m_LastBakedAtmosphere) ||
                            resolvedView.SunDirection != m_LastBakedAtmosphereSun;
            }

            // The SH tier's self-contained readback bake runs first, before the frame's display bake
            // records into cmd: its immediate submit (bake + copy + download) records its barriers
            // off the persistent image-layout tracker, so it must see the tracker as the display
            // bake left it last frame, not as this frame's not-yet-submitted display bake would. It
            // leaves the cube freshly baked in a sampled layout, which the display bake below then
            // re-records over into cmd. Both ride the bake dirty signal, so a static sky pays once.
            if (m_ResolvedSkyLighting == SkyLighting::SH)
            {
                if (bakeDirty || !m_SkyShValid)
                {
                    const vector<u8> faces =
                        bakedMaterial
                            ? m_SkyBake->BakeAndDownload(*material)
                            : m_SkyBake->BakeAtmosphereAndDownload(
                                  m_SkyPipeline, m_Atmosphere->GetSet(), resolvedView.Atmosphere,
                                  resolvedView.SunDirection, resolvedView.AtmosphereIntensity);
                    m_SkySh =
                        EnvironmentIbl::ProjectCubeToIrradianceSh(faces, m_SkyBake->GetFaceSize());
                    m_SkyShValid = true;
                }
            }

            // The display bake into the frame command buffer, so the skybox pass samples the cube
            // this frame. Recorded after the SH readback (above) and before the IBL convolution
            // (below), which reads the cube this bake fills.
            if (bakeDirty)
            {
                if (bakedMaterial)
                {
                    m_SkyBake->Bake(cmd, *material);
                }
                else
                {
                    m_SkyBake->BakeAtmosphere(cmd, m_SkyPipeline, m_Atmosphere->GetSet(),
                                              resolvedView.Atmosphere, resolvedView.SunDirection,
                                              resolvedView.AtmosphereIntensity);
                    m_LastBakedAtmosphere = resolvedView.Atmosphere;
                    m_LastBakedAtmosphereSun = resolvedView.SunDirection;
                    m_BakedAtmosphereValid = true;
                }
                m_LastBakedSkyMaterial = material;
                m_LastBakedSkyMaterialRevision = material != nullptr ? material->GetRevision() : 0;
            }

            // IBL convolves the freshly-filled bake cube into the split-sum maps in this command
            // buffer, on the same bake dirty signal, so a static sky pays it once.
            if (m_ResolvedSkyLighting == SkyLighting::IBL)
            {
                if (bakeDirty || !m_SkyCubeConvolved)
                {
                    m_Ibl->EnsureInitialized(cmd);
                    m_Ibl->GenerateFromCube(cmd, m_SkyBake->GetCubeView(),
                                            m_SkyBake->GetFaceSize());
                    m_SkyCubeConvolved = true;
                }
            }
            else
            {
                m_SkyCubeConvolved = false;
            }
        }
        else
        {
            m_LastBakedSkyMaterial = nullptr;
            m_SkyCubeConvolved = false;
            m_BakedAtmosphereValid = false;
        }

        // An environment sky on the SH tier lights the diffuse term from its radiance cube — the
        // same cube the skybox samples. Project it to the skylight coefficients on the
        // environment-change signal, before the view-constants write below folds m_SkySh in; a
        // static environment projects once. The environment IBL tier convolves in the main command
        // buffer below (its skybox radiance is already produced there).
        if (m_ResolvedSkyKind == SkySourceKind::Environment &&
            m_ResolvedSkyLighting == SkyLighting::SH && resolvedView.Environment.IsLoaded())
        {
            const EnvironmentMap* environment = resolvedView.Environment.Get();
            if (environment != m_LastSkyShEnvironment)
            {
                m_SkySh = m_Ibl->ProjectEnvironmentToIrradianceSh(*environment);
                m_SkyShValid = true;
                m_LastSkyShEnvironment = environment;
            }
        }
        else
        {
            m_LastSkyShEnvironment = nullptr;
        }

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
        // SH skylight: the lighting pass's second ambient arm reads m_SkySh from the view-constants
        // block below. Every SH-tier source projects the one radiance cube it fills — an environment
        // its equirect cube, a baked material or baked atmosphere its bake cube — on that source's
        // dirty signal above; the projection is a pure cube→SH read, so display and ambient agree.
        const bool skylightActive = m_SkylightActive;

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
            .SceneColor = uvec4(m_RefractionSceneHandle.Index, m_SamplerHandle.Index,
                                m_RefractionActive ? 1u : 0u, m_RefractionDepthHandle.Index),
        };
        for (u32 i = 0; i < ShCoefficientCount; ++i)
        {
            viewConstants.SkyShCoeffs[i] =
                skylightActive ? vec4(m_SkySh.Coefficients[i], 0.0f) : vec4(0.0f);
        }
        registry.WriteViewConstants(std::as_bytes(std::span(&viewConstants, 1)));

        // Decide whether last frame's pyramid is trustworthy this frame. The reset
        // flag (frame 0 / post-resize / post-configure) forces invalid regardless of
        // the view delta; otherwise the device-free metric compares this frame's
        // camera against last frame's. The result feeds the GPU cull (occlusion skipped
        // when invalid); this plan lands it for tests and the cull to consume.
        const mat4 invView = glm::inverse(view.Camera.View());
        const HiZHistoryState currentHiZState{
            .CameraPosition = view.Camera.GetPosition(),
            // The camera looks down -Z in view space; its world forward is the negated
            // third basis column of the view's inverse.
            .CameraForward = glm::normalize(-vec3(invView[2])),
            .Projection = view.Camera.Projection(),
        };
        const f32 sceneDiagonal = sceneBounds.IsEmpty() ? 0.0f : glm::length(sceneBounds.Size());
        m_HiZHistoryValid =
            !m_HiZHistoryReset && Renderer::IsHiZHistoryValid(m_PreviousHiZState, currentHiZState,
                                                              sceneDiagonal, HiZHistorySettings{});

        // Pack set-1 ShadowConstants: tile-remapped cascade view-projs, splits, and
        // params. Enabled only when the shadow pass is wired AND a directional light
        // exists this frame; otherwise the lighting pass reads full visibility.
        const bool shadowEnabled = m_ShadowActive && m_ShadowPass && packed.HaveDirectional;
        const ShadowAtlasGrid grid = ComputeShadowAtlasGrid(m_Settings.CascadeCount);

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
            vec4(1.0f / static_cast<f32>(m_Settings.ShadowResolution), blendBand,
                 static_cast<f32>(cascades.Count), shadowEnabled ? 1.0f : 0.0f);

        // Write this frame's shadow + punctual ring regions (not yet submitted; safe).
        const u32 frameIndex = m_Context.GetCurrentFrameInFlight();
        m_Shadows->WriteFrameConstants(frameIndex, shadowConstants, punctualBlock);

        // Read the GPU survivor count the previous Execute wrote into this frame's region
        // before PrepareDraws zeroes it again — the host-visible count is one frame late, so
        // it never gates this frame's draw. Only meaningful under the GPU path.
        if (m_ActiveCull == SceneRendererSettings::CullMode::GPU && m_CullCountBuffer)
        {
            const auto* counts = static_cast<const u32*>(m_CullCountBuffer->GetMappedData());
            m_LastGpuSurvivorCount = counts[frameIndex];
        }

        // Fill the per-draw DrawData buffer + (GPU) the candidate buffer and submission plan.
        // The surface push's ViewConstantsIndex is the shared per-view slot, distinct from the
        // renderer-owned frame-in-flight rings PrepareDraws indexes internally.
        PrepareDraws(resolvedView, registry.GetCurrentViewConstantsIndex());

        // Build the id-writing pipeline variants on the first frame a surface material is available
        // (their layout is shared across surface materials), so the picking pass can re-draw the
        // same survivors. A no-op when picking is off or the pipelines are already built.
        if (m_PickingActive)
        {
            EnsurePickingPipelines(m_Internal->Plan.PipelineMaterial,
                                   m_Internal->Plan.SkinnedPipelineMaterial);
        }

        // Expose the skinning palette + per-entity bases (filled by PrepareDraws) to the shadow
        // passes so a skinned caster casts its posed shadow.
        resolvedView.SkinningPalette = m_PaletteSet;
        resolvedView.SkinnedPaletteBases = &m_PaletteBaseByEntity;

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
        if (m_PickingActive)
        {
            bindings.push_back({m_EntityIdId, m_EntityIdView});
            bindings.push_back({m_PickingDepthId, m_PickingDepthView});
        }
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
            bindings.push_back({m_RefractionSceneId, m_RefractionSceneView});
            bindings.push_back({m_RefractionDepthId, m_RefractionDepthView});
        }
        if (m_SsrActive)
        {
            bindings.push_back({m_SsrSceneId, m_SsrSceneView});
            // Each reflection mip binds its per-frame view to its per-mip import slot (the trace
            // writes mip 0, the blur the rest).
            for (u32 level = 0; level < m_SsrReflectionMips.size(); level++)
            {
                bindings.push_back(
                    {m_SsrReflectionChainId.Level(level), m_SsrReflectionMips[level]});
            }
            for (u32 level = 0; level < m_SsrHiZMips.size(); level++)
            {
                bindings.push_back({m_SsrHiZChainId.Level(level), m_SsrHiZMips[level]});
            }
        }
        // Bind each hi-Z mip's per-frame storage view to its per-mip import slot.
        for (u32 level = 0; level < m_HiZMips.size(); level++)
        {
            bindings.push_back({m_HiZChainId.Level(level), m_HiZMips[level]});
        }
        // The GPU cull arm shares the indirect command buffer between the cull pass and the
        // geometry pass through this import (the same buffer the cull set binding 2 writes).
        if (m_ActiveCull == SceneRendererSettings::CullMode::GPU)
        {
            bindings.push_back({.Id = m_IndirectId, .Buffer = m_IndirectBuffer});
        }
        // Image-based lighting: initialize the BRDF LUT + leave the maps in a sampled layout
        // once, then (re)generate the radiance/irradiance/prefilter maps when the bound
        // environment changes — a one-time cost recorded before the graph the lighting pass
        // samples them through. Recorded into cmd before the graph so the cubes are resident
        // and in their sampled layout when the lighting pass runs.
        m_Ibl->EnsureInitialized(cmd);
        const EnvironmentMap* environment =
            resolvedView.Environment.IsLoaded() ? resolvedView.Environment.Get() : nullptr;
        if (environment != nullptr && environment != m_LastEnvironment)
        {
            m_Ibl->Generate(cmd, *environment);
        }
        m_LastEnvironment = environment;

        // The atmosphere LUTs were generated ahead of the atmosphere bake, before the frame's
        // BeginView (a baked atmosphere reads them per face); nothing more to record here.

        m_Internal->Graph->Execute(cmd, bindings, &resolvedView);

        // Service a pending pick: the picking pass left the EntityId target in ColorAttachment
        // layout, so transition it to TransferSrc and copy the search window under the cursor into
        // this frame's readback region. The result becomes readable once this frame's GPU work
        // completes (PollPickId waits frames-in-flight); the copy rides the graphics queue, no stall.
        if (m_PickRequested && !m_PickStaged && m_PickingActive && m_EntityIdImage)
        {
            const u32 window = static_cast<u32>(2 * Picking::SearchRadius + 1);
            // Clamp the window into the target; the cursor's offset within it is recorded so the
            // search-radius logic measures distance from the real cursor texel, not the clamped one.
            const uvec2 maxOrigin = uvec2(m_Extent.x - 1, m_Extent.y - 1);
            const uvec2 clampedTexel = glm::min(m_PickTexel, maxOrigin);
            const uvec2 origin{
                clampedTexel.x >= static_cast<u32>(Picking::SearchRadius)
                    ? std::min(clampedTexel.x - static_cast<u32>(Picking::SearchRadius),
                               m_Extent.x - window)
                    : 0u,
                clampedTexel.y >= static_cast<u32>(Picking::SearchRadius)
                    ? std::min(clampedTexel.y - static_cast<u32>(Picking::SearchRadius),
                               m_Extent.y - window)
                    : 0u,
            };
            const uvec2 copyExtent{std::min(window, m_Extent.x), std::min(window, m_Extent.y)};

            cmd.PrepareForAccess(m_EntityIdView, AccessKind::TransferSrc);
            // The copy lands tightly packed at buffer byte 0 as a copyExtent.x-wide grid; the host
            // reads it once this frame completes (PollPickId waits frames-in-flight).
            cmd.CopyImageRegionToBuffer(m_EntityIdImage, m_PickReadbackBuffer, origin, copyExtent);

            m_PickWindowOrigin = origin;
            m_PickWindowExtent = copyExtent;
            m_PickCursorInWindow = clampedTexel - origin;
            m_PickStaged = true;
            m_PickStagedFrame = m_FrameIndex;
        }

        // Capture this frame's camera + matrix for next frame's history comparison and
        // occlusion test: the reduction declared in this graph wrote the pyramid from
        // this frame's depth, so it pairs with this frame's view-projection next time.
        m_PreviousViewProj = viewProj;
        m_PreviousHiZState = currentHiZState;

        // Record this frame's sub-rect mapping: GetValidExtent reports it, and the next frame's
        // TAA resolve reprojects the history written at this scale (the zw of RenderScaleUV).
        m_ValidExtent = validExtent;
        m_PreviousRenderScaleUV = renderScaleUV;
        m_PreviousMaxValidUV = maxValidUV;
        // The pyramid now holds this frame's depth, so the next Execute may test against
        // it (subject to the view-delta metric).
        m_HiZHistoryReset = false;

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
        return m_ActiveCull;
    }
    u32 SceneRenderer::GetLastGpuSurvivorCount() const
    {
        return m_LastGpuSurvivorCount;
    }
    vector<u32> SceneRenderer::ReadbackGpuSurvivorFlags() const
    {
        if (!m_GpuReadbackValid || m_GpuCandidateCount == 0 || !m_IndirectBuffer)
        {
            return {};
        }

        // Download the full indirect buffer and pull each candidate command's instanceCount
        // from this frame's region; 1 = drawn, 0 = occluded.
        const vector<u8> bytes = m_IndirectBuffer->Download();
        const auto* commands = reinterpret_cast<const DrawIndexedIndirectCommand*>(bytes.data());
        const u32 base = m_GpuReadbackRegion * MaxCullCandidates;

        vector<u32> flags(m_GpuCandidateCount);
        for (u32 i = 0; i < m_GpuCandidateCount; ++i)
        {
            flags[i] = commands[base + i].InstanceCount;
        }
        return flags;
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
        return m_AtmosphereRegeneratedLastFrame;
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
        return m_HiZSampleView;
    }
    Ref<ImageView> SceneRenderer::GetHiZMipView(const u32 level) const
    {
        VE_ASSERT(level < m_HiZMips.size(), "SceneRenderer::GetHiZMipView: level {} out of range",
                  level);
        return m_HiZMips[level];
    }
    u32 SceneRenderer::GetHiZMipCount() const
    {
        return static_cast<u32>(m_HiZMips.size());
    }
    bool SceneRenderer::IsHiZHistoryValid() const
    {
        return m_HiZHistoryValid;
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
        if (!m_Settings.Picking)
        {
            return;
        }
        // A new request replaces any in-flight one (the latest click wins).
        m_PickTexel = texel;
        m_PickRequested = true;
        m_PickStaged = false;
    }

    bool SceneRenderer::IsPickInFlight() const
    {
        return m_PickRequested;
    }

    optional<u32> SceneRenderer::PollPickId()
    {
        if (!m_PickRequested || !m_PickStaged)
        {
            return std::nullopt;
        }
        // The staged copy is safe to read once its frame's GPU work has retired — at least
        // GetMaxFramesInFlight() Executes after it was recorded (the same deferral the retire
        // path uses). Until then the readback is still pending.
        if (m_FrameIndex - m_PickStagedFrame < m_Context.GetMaxFramesInFlight())
        {
            return std::nullopt;
        }

        // Search the staged window: the exact cursor texel wins when non-zero; otherwise the
        // nearest non-zero id to the cursor (front-most resolution rides the depth test the
        // picking pass already applied, so a non-zero texel is already the nearest surface there).
        const auto* ids = static_cast<const u32*>(m_PickReadbackBuffer->GetMappedData());
        const u32 width = m_PickWindowExtent.x;
        const u32 height = m_PickWindowExtent.y;

        u32 resolved = Picking::NoEntityId;
        const uvec2 cursor = m_PickCursorInWindow;
        const u32 exact = (cursor.x < width && cursor.y < height) ? ids[cursor.y * width + cursor.x]
                                                                  : Picking::NoEntityId;
        if (exact != Picking::NoEntityId)
        {
            resolved = exact;
        }
        else
        {
            u64 bestDistanceSq = ~0ull;
            for (u32 y = 0; y < height; ++y)
            {
                for (u32 x = 0; x < width; ++x)
                {
                    const u32 id = ids[y * width + x];
                    if (id == Picking::NoEntityId)
                    {
                        continue;
                    }
                    const i64 dx = static_cast<i64>(x) - static_cast<i64>(cursor.x);
                    const i64 dy = static_cast<i64>(y) - static_cast<i64>(cursor.y);
                    const u64 distanceSq = static_cast<u64>(dx * dx + dy * dy);
                    if (distanceSq < bestDistanceSq)
                    {
                        bestDistanceSq = distanceSq;
                        resolved = id;
                    }
                }
            }
        }

        // Consume the request: the caller takes the result.
        m_PickRequested = false;
        m_PickStaged = false;
        return resolved;
    }
}
