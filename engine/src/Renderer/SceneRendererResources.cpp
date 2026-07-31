// The Create phase of SceneRenderer's Create / Resize / Configure / Execute lifetime split: the
// six members that allocate the renderer's persistent resources — the LTC LUTs, the fullscreen
// pipelines, the output target, the g-buffer, the GPU-cull buffers, and the HDR target. They are
// SceneRenderer members compiled in their own translation unit, leaving SceneRenderer.cpp the
// per-frame and graph-wiring paths. Resize and Configure call these but are orchestration, not
// construction, so they stay with the wiring.

#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/LtcLut.h>
#include <Veng/Asset/RawAsset.h>

#include "DebugBlitPipelines.h"
#include "EnvironmentIbl.h"
#include "AtmospherePrecompute.h"
#include "GpuBlocks.h"
#include "GpuCullSystem.h"
#include "Passes/DeferredLightingScenePass.h"
#include "SceneRendererIds.h"
#include "ShadowSystem.h"
#include "SkyResolver.h"

#include <span>

#include <Veng/Assert.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/GBuffer.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/Sampler.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Shader.h>

namespace Veng::Renderer
{
    namespace
    {
        // The engine core pack's fullscreen shaders (the AssetManager auto-mounts the core pack).
        constexpr AssetId DeferredLightingFragId{0x6569EBAC0810CC1FULL};
        constexpr AssetId DeferredLightingSsaoFragId{0x6EEF5D26BAF2849FULL};
        constexpr AssetId DeferredLightingCascadesFragId{0x834ED7C05F336E01ULL};
        constexpr AssetId SkyboxFragId{0xFCA568CC3463618FULL};
        constexpr AssetId AtmosphereSkyFragId{0x7DC6D927B2DF7858ULL};
        // The baked LTC lookup tables (matrix table then magnitude table, RGBA32F) for area lights.
        constexpr AssetId LtcLutId{0x27644C3AE58BB0D3ULL};

        constexpr AssetId SsaoFragId{0xCCBA63DB760A4E8EULL};
        constexpr AssetId TonemapInstanceId{0xB5AA7227E8A2DC11ULL};
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

        if (!m_SamplerHandle.IsValid())
        {
            m_SamplerHandle = bindless
                                  .AcquireSampler({
                                      .Name = "SceneRenderer GBuffer Sampler",
                                      .AddressModeU = AddressMode::ClampToEdge,
                                      .AddressModeV = AddressMode::ClampToEdge,
                                      .AddressModeW = AddressMode::ClampToEdge,
                                  })
                                  .Handle;
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
}
