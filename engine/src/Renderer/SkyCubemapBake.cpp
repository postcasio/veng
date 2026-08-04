#include "SkyCubemapBake.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include <fmt/format.h>

#include <Veng/Assert.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/GeneratedTextureService.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/Sampler.h>

namespace Veng::Renderer
{
    namespace
    {
        // The IBL consumer set's bindings the skybox pipeline reads: the radiance cube at 0, the
        // linear sampler at 4. The bake writes its cube + sampler into a set with this layout so
        // the skybox pass samples the baked cube exactly as it samples the environment radiance.
        constexpr u32 RadianceBinding = 0;
        constexpr u32 SamplerBinding = 4;

        // A process-unique key per bake instance, so two SkyCubemapBakes sharing one context's
        // generated-texture service never collide on the service's key space.
        GeneratedTextureKey NextSkyBakeJobKey()
        {
            static std::atomic<u64> counter{0};
            return (0x536B'7942'0000'0000ull) | counter.fetch_add(1, std::memory_order_relaxed);
        }

        // The Sky material's per-draw push block (Veng/sky.slang SkyPushConstants): the frame-folded
        // material selector, the g-buffer depth handle + sampler, and the view-constants region the
        // fragment reconstructs its per-pixel view ray from. The bake fills the depth slot with a
        // far-plane stand-in and the view-constants region with the face basis.
        struct SkyMaterialPushConstants
        {
            u32 MaterialIndex;
            u32 DepthTexture;
            u32 DepthSampler;
            u32 ViewConstantsIndex;
        };

        // The atmosphere sky fragment's push block (atmosphere_sky.frag.slang PushConstants): the
        // depth handle + sampler, the view-constants region, an Enabled flag, then the sun direction
        // + intensity and the AtmosphereParams block. Mirrors it byte-for-byte, including the
        // trailing float3 pad, so the fragment binds and pushes exactly as in the direct path. The
        // bake fills the depth slot with the far-plane stand-in, the view-constants region with the
        // face basis, and Enabled with 1 (a baked sky always evaluates every face).
        struct AtmosphereSkyPushConstants
        {
            u32 DepthTexture;
            u32 Sampler;
            u32 ViewConstantsIndex;
            u32 Enabled;
            vec3 SunDirection;
            f32 Intensity;
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

        // A full view-constants region (set 0 binding 5), mirroring view_constants.slang byte-for-byte
        // so the material fragment's LoadViewConstants reads the face basis. Only InvViewProj +
        // CameraPosition + the sub-rect mapping matter to a sky; the rest is zero.
        struct ViewConstantsRegion
        {
            mat4 InvViewProj;
            mat4 InvViewRotProj;
            vec4 CameraPosition;
            mat4 View;
            mat4 Proj;
            mat4 PrevViewProj;
            mat4 CurViewProj;
            vec4 RenderScaleUV;
            vec4 MaxValidUV;
            std::array<vec4, 9> SkyShCoeffs;
            // Zeroed: a baked source is static (no frame clock) and has no scene behind it. The
            // fields still belong in the write so a bake face's ring slot never inherits a scene
            // view's stale handles.
            vec4 TimeParams;
            vec4 ExtentParams;
            uvec4 SceneColor;
        };

        static_assert(sizeof(ViewConstantsRegion) <= BindlessRegistry::ViewConstantsStride,
                      "ViewConstantsRegion must fit one ring-buffered view-constants region");

        // The per-face InvViewProj: maps a fullscreen [0,1]² UV, unprojected as (st.x, st.y, 1, 1)
        // with st = uv*2-1, to the face's world direction. The mapping is built directly from the
        // face basis (A the st.x coefficient, B the st.y coefficient, C the constant) so the
        // reconstructed direction equals the cube image-view layer convention (ibl_equirect_to_cube's
        // FaceDirection) exactly — the twelve shared edges then evaluate one direction, so the cube
        // is seamless by construction. Column-major: col0 = A, col1 = B, col2 = 0, col3 = (C, 1),
        // so InvViewProj * (st.x, st.y, 1, 1) = (A·st.x + B·st.y + C, 1).
        mat4 FaceInvViewProj(const vec3& a, const vec3& b, const vec3& c)
        {
            mat4 m(0.0f);
            m[0] = vec4(a, 0.0f);
            m[1] = vec4(b, 0.0f);
            m[2] = vec4(0.0f);
            m[3] = vec4(c, 1.0f);
            return m;
        }

        // Halving steps from a face of the given size down to the SH readback size: the readback
        // level's mip index, and one less than the cube's mip count. Halts at the readback size or
        // the first odd level, so a non-power-of-two face reduces as far as it evenly can.
        u32 ComputeShReadbackMip(const u32 faceSize)
        {
            u32 mip = 0;
            u32 size = faceSize;
            while (size > SkyCubemapBake::ShReadbackFaceSize && (size % 2 == 0))
            {
                size /= 2;
                ++mip;
            }
            return mip;
        }

        std::array<mat4, SkyCubemapBake::CubeFaces> BuildFaceMatrices()
        {
            return {
                FaceInvViewProj({0, 0, -1}, {0, -1, 0}, {1, 0, 0}),  // +X
                FaceInvViewProj({0, 0, 1}, {0, -1, 0}, {-1, 0, 0}),  // -X
                FaceInvViewProj({1, 0, 0}, {0, 0, 1}, {0, 1, 0}),    // +Y
                FaceInvViewProj({1, 0, 0}, {0, 0, -1}, {0, -1, 0}),  // -Y
                FaceInvViewProj({1, 0, 0}, {0, -1, 0}, {0, 0, 1}),   // +Z
                FaceInvViewProj({-1, 0, 0}, {0, -1, 0}, {0, 0, -1}), // -Z
            };
        }
    }

    Unique<SkyCubemapBake> SkyCubemapBake::Create(Context& context,
                                                  const Ref<DescriptorSetLayout>& consumerLayout,
                                                  const Format sceneColorFormat, const u32 faceSize)
    {
        return Unique<SkyCubemapBake>(
            new SkyCubemapBake(context, consumerLayout, sceneColorFormat, faceSize));
    }

    SkyCubemapBake::SkyCubemapBake(Context& context, const Ref<DescriptorSetLayout>& consumerLayout,
                                   const Format sceneColorFormat, const u32 faceSize)
        : m_Context(context), m_SceneColorFormat(sceneColorFormat), m_FaceSize(faceSize),
          m_ShReadbackMip(ComputeShReadbackMip(faceSize)), m_FaceInvViewProj(BuildFaceMatrices())
    {
        // The radiance cube: six layers rendered as color attachments, sampled as a cube. Uses the
        // scene-color format so the baked radiance round-trips the skybox sampler with no conversion.
        // Carries a mip chain down to the SH readback level, filled by a box-averaging blit chain
        // (so the reduction is both a transfer source and destination); the skybox still samples
        // mip 0 alone.
        m_CubeImage = Image::Create(m_Context,
                                    {
                                        .Name = "Sky Bake Radiance Cube",
                                        .Extent = {m_FaceSize, m_FaceSize, 1},
                                        .MipLevels = m_ShReadbackMip + 1,
                                        .Layers = CubeFaces,
                                        .Format = m_SceneColorFormat,
                                        .Usage = ImageUsage::Sampled | ImageUsage::ColorAttachment |
                                                 ImageUsage::TransferSrc | ImageUsage::TransferDst,
                                    });
        // The skybox samples mip 0 only — the reduction chain is a readback path, not a display
        // change — so the sampled cube view exposes a single mip level.
        m_CubeView = ImageView::Create(m_Context, {
                                                      .Name = "Sky Bake Radiance Cube View",
                                                      .Image = m_CubeImage,
                                                      .ViewType = ImageViewType::Cube,
                                                      .MipLevels = 1,
                                                      .ArrayLayers = CubeFaces,
                                                  });
        for (u32 face = 0; face < CubeFaces; ++face)
        {
            m_FaceViews[face] =
                ImageView::Create(m_Context, {
                                                 .Name = fmt::format("Sky Bake Face {} View", face),
                                                 .Image = m_CubeImage,
                                                 .ViewType = ImageViewType::Type2D,
                                                 .BaseArrayLayer = face,
                                                 .ArrayLayers = 1,
                                             });
        }
        // One all-six-layers view per mip level, for the reduction/readback layout transitions (the
        // raw blit addresses subresources directly, but PrepareForAccess tracks through a view).
        m_MipViews.reserve(m_ShReadbackMip + 1);
        for (u32 mip = 0; mip <= m_ShReadbackMip; ++mip)
        {
            m_MipViews.push_back(
                ImageView::Create(m_Context, {
                                                 .Name = fmt::format("Sky Bake Mip {} View", mip),
                                                 .Image = m_CubeImage,
                                                 .ViewType = ImageViewType::Array2D,
                                                 .BaseMipLevel = mip,
                                                 .MipLevels = 1,
                                                 .BaseArrayLayer = 0,
                                                 .ArrayLayers = CubeFaces,
                                             }));
        }

        // The scratch cube an amortized bake fills a face at a time. Mip 0 only — it is copied into
        // the displayed cube (which owns the reduction chain) once every face has been rendered, so
        // it needs no mips of its own. TransferSrc for that copy; the service ORs in the rest.
        m_ScratchImage = Image::Create(m_Context, {
                                                      .Name = "Sky Bake Scratch Cube",
                                                      .Extent = {m_FaceSize, m_FaceSize, 1},
                                                      .MipLevels = 1,
                                                      .Layers = CubeFaces,
                                                      .Format = m_SceneColorFormat,
                                                      .Usage = ImageUsage::Sampled |
                                                               ImageUsage::ColorAttachment |
                                                               ImageUsage::TransferSrc,
                                                  });
        m_ScratchView = ImageView::Create(m_Context, {
                                                         .Name = "Sky Bake Scratch Cube View",
                                                         .Image = m_ScratchImage,
                                                         .ViewType = ImageViewType::Array2D,
                                                         .ArrayLayers = CubeFaces,
                                                     });
        for (u32 face = 0; face < CubeFaces; ++face)
        {
            m_ScratchFaceViews[face] = ImageView::Create(
                m_Context, {
                               .Name = fmt::format("Sky Bake Scratch Face {} View", face),
                               .Image = m_ScratchImage,
                               .ViewType = ImageViewType::Type2D,
                               .BaseArrayLayer = face,
                               .ArrayLayers = 1,
                           });
        }
        m_JobKey = NextSkyBakeJobKey();

        // A 1×1 stand-in depth image holding the far-plane value (1.0), bound in the fragment's depth
        // slot so SkyIsBackground passes for every baked pixel — there is no g-buffer at bake time.
        // Sampled as a color texture (the fragment reads .r), so it is an R32Sfloat sampled image.
        m_DepthImage =
            Image::Create(m_Context, {
                                         .Name = "Sky Bake Stand-in Depth",
                                         .Extent = {1, 1, 1},
                                         .Format = Format::R32Sfloat,
                                         .Usage = ImageUsage::Sampled | ImageUsage::TransferDst,
                                     });
        constexpr f32 farPlane = 1.0f;
        m_DepthImage->UploadSync(
            std::span<const u8>(reinterpret_cast<const u8*>(&farPlane), sizeof(farPlane)));
        m_DepthView = ImageView::Create(
            m_Context, {.Name = "Sky Bake Stand-in Depth View", .Image = m_DepthImage});
        m_DepthHandle = m_Context.GetBindlessRegistry().Register(m_DepthView);
        m_DepthSamplerHandle = m_Context.GetBindlessRegistry()
                                   .AcquireSampler({
                                       .Name = "Sky Bake Depth Sampler",
                                       .AddressModeU = AddressMode::ClampToEdge,
                                       .AddressModeV = AddressMode::ClampToEdge,
                                       .AddressModeW = AddressMode::ClampToEdge,
                                   })
                                   .Handle;

        // The consumer set the skybox pass binds: the baked cube at the radiance binding, plus a
        // linear clamp sampler at the sampler binding — the IBL consumer set's shape for those two.
        const Ref<Sampler> sampler =
            Sampler::Create(m_Context, {
                                           .Name = "Sky Bake Sampler",
                                           .MagFilter = Filter::Linear,
                                           .MinFilter = Filter::Linear,
                                           .AddressModeU = AddressMode::ClampToEdge,
                                           .AddressModeV = AddressMode::ClampToEdge,
                                           .AddressModeW = AddressMode::ClampToEdge,
                                           .AnisotropyEnabled = false,
                                       });
        m_ConsumerSet = DescriptorSet::Create(
            m_Context, {.Name = "Sky Bake Consumer Set", .Layout = consumerLayout});
        m_ConsumerSet->Write(RadianceBinding, m_CubeView);
        m_ConsumerSet->Write(SamplerBinding, sampler);

        // Clear the displayed cube to black and leave it sampled, so the skybox pass samples a
        // defined cube from the very first frame — the amortized fill lands a bake into it a few
        // frames later, and before that there is nothing baked to show. The synchronous bake used to
        // fill it in the frame it was first sampled; the amortized fill does not, so it is seeded
        // here instead.
        m_Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                cmd.PrepareForAccess(m_CubeView, AccessKind::TransferDst);
                const vk::ClearColorValue black{std::array<f32, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
                const vk::ImageSubresourceRange range{.aspectMask = vk::ImageAspectFlagBits::eColor,
                                                      .baseMipLevel = 0,
                                                      .levelCount = 1,
                                                      .baseArrayLayer = 0,
                                                      .layerCount = CubeFaces};
                GetVkCommandBuffer(cmd).clearColorImage(GetVkImage(*m_CubeImage),
                                                        vk::ImageLayout::eTransferDstOptimal,
                                                        &black, 1, &range);
                cmd.PrepareForAccess(m_CubeView, AccessKind::Sample);
            });
    }

    SkyCubemapBake::~SkyCubemapBake()
    {
        // Tear down any amortized bake still in flight so its tick/completion callbacks — which
        // capture this — never fire after it is gone.
        CancelBake();
        m_Context.GetBindlessRegistry().Release(m_DepthHandle);
    }

    void SkyCubemapBake::EnsurePipeline(const MaterialInstance& material)
    {
        if (m_Pipeline && m_PipelineMaterialIndex == material.GetIndex())
        {
            return;
        }

        VE_ASSERT(material.GetDomain() == MaterialDomain::Sky,
                  "SkyCubemapBake: material '{}' is not a Sky material", material.GetName());

        // The material's own fragment + the fullscreen vertex, against the cube-face color format.
        // The layout (set 0 reserved, the sky push range) comes from the material loader, so the
        // fragment binds and pushes exactly as in the direct path — it is unchanged and unaware of
        // the bake.
        m_Pipeline = GraphicsPipeline::Create(
            m_Context,
            {
                .Name = fmt::format("Sky Bake Pipeline ({})", material.GetName()),
                .ColorAttachments = {{.Format = m_SceneColorFormat}},
                .PipelineLayout = material.GetPipelineLayout(),
                .ShaderStages =
                    {
                        {.Stage = ShaderStage::Vertex, .Module = material.GetVertexModule()},
                        {.Stage = ShaderStage::Fragment, .Module = material.GetFragmentModule()},
                    },
            });
        m_PipelineMaterialIndex = material.GetIndex();
    }

    void SkyCubemapBake::Bake(CommandBuffer& cmd, const MaterialInstance& material)
    {
        EnsurePipeline(material);

        BindlessRegistry& registry = m_Context.GetBindlessRegistry();
        const u32 selector = material.GetMaterialSelector();

        // Render each face into its single-layer view with the face's fixed basis in the view
        // constants and the far-plane stand-in in the depth slot. Each face claims its own view slot
        // so the six face constants coexist; the material fragment reconstructs the ray from the slot
        // the face pushes. The caller reserved the whole cube's slots, so a refused claim here is a
        // broken precondition rather than a budget the frame can degrade against.
        for (u32 face = 0; face < CubeFaces; ++face)
        {
            const bool claimed = registry.TryBeginView();
            VE_ASSERT(claimed,
                      "SkyCubemapBake::Bake: the frame's view budget does not cover {} cube faces; "
                      "the caller must reserve them before recording a bake",
                      CubeFaces);
            ViewConstantsRegion region{};
            region.InvViewProj = m_FaceInvViewProj[face];
            // The face basis is already a pure direction mapping with no camera translation in it,
            // so it is its own rotation-only inverse and serves SkyViewDirection unchanged.
            region.InvViewRotProj = m_FaceInvViewProj[face];
            region.CameraPosition = vec4(0.0f);
            region.RenderScaleUV = vec4(1.0f, 1.0f, 1.0f, 1.0f);
            region.MaxValidUV = vec4(1.0f, 1.0f, 1.0f, 1.0f);
            registry.WriteViewConstants(std::as_bytes(std::span(&region, 1)));
            const u32 viewIndex = registry.GetCurrentViewConstantsIndex();

            cmd.PrepareForAccess(m_FaceViews[face], AccessKind::ColorAttachment);
            cmd.BeginRendering({
                .Extent = {m_FaceSize, m_FaceSize},
                .ColorAttachments = {{
                    .ImageView = m_FaceViews[face],
                    .LoadOp = LoadOp::Clear,
                    .StoreOp = StoreOp::Store,
                    .ClearValue = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
                }},
            });
            cmd.BindPipeline(m_Pipeline);
            cmd.SetViewport({0, 0}, {m_FaceSize, m_FaceSize});
            cmd.SetScissor({0, 0}, {m_FaceSize, m_FaceSize});
            registry.Bind(cmd);
            cmd.PushConstants(SkyMaterialPushConstants{
                .MaterialIndex = selector,
                .DepthTexture = m_DepthHandle.Index,
                .DepthSampler = m_DepthSamplerHandle.Index,
                .ViewConstantsIndex = viewIndex,
            });
            cmd.DrawFullscreenTriangle();
            cmd.EndRendering();
        }
        m_FaceRendersRecorded += CubeFaces;

        // Leave the whole cube in a sampled layout for the skybox pass that samples it this frame.
        cmd.PrepareForAccess(m_CubeView, AccessKind::Sample);
    }

    void SkyCubemapBake::RecordReductionMips(CommandBuffer& cmd)
    {
        // The display face already sits at or below the readback size; the readback reads mip 0.
        if (m_ShReadbackMip == 0)
        {
            return;
        }

        u32 width = m_FaceSize;
        u32 height = m_FaceSize;
        for (u32 dst = 1; dst <= m_ShReadbackMip; ++dst)
        {
            const u32 src = dst - 1;
            cmd.PrepareForAccess(m_MipViews[src], AccessKind::TransferSrc);
            cmd.PrepareForAccess(m_MipViews[dst], AccessKind::TransferDst);

            const u32 dstWidth = std::max(1u, width / 2);
            const u32 dstHeight = std::max(1u, height / 2);
            // One blit downsamples all six layers at once; a linear filter over a 2× reduction is a
            // 2×2 box average, so the chain reaches the readback level as a spherical-integral-
            // preserving reduction rather than a point resample.
            const vk::ImageBlit blit{
                .srcSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                   .mipLevel = src,
                                   .baseArrayLayer = 0,
                                   .layerCount = CubeFaces},
                .srcOffsets = {{vk::Offset3D{.x = 0, .y = 0, .z = 0},
                                vk::Offset3D{.x = static_cast<i32>(width),
                                             .y = static_cast<i32>(height),
                                             .z = 1}}},
                .dstSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                   .mipLevel = dst,
                                   .baseArrayLayer = 0,
                                   .layerCount = CubeFaces},
                .dstOffsets = {{vk::Offset3D{.x = 0, .y = 0, .z = 0},
                                vk::Offset3D{.x = static_cast<i32>(dstWidth),
                                             .y = static_cast<i32>(dstHeight),
                                             .z = 1}}},
            };
            GetVkCommandBuffer(cmd).blitImage(
                GetVkImage(*m_CubeImage), vk::ImageLayout::eTransferSrcOptimal,
                GetVkImage(*m_CubeImage), vk::ImageLayout::eTransferDstOptimal, 1, &blit,
                vk::Filter::eLinear);

            width = dstWidth;
            height = dstHeight;
        }

        // Restore mip 0 (the display cube) to a sampled layout for the skybox pass this frame; the
        // readback level is left in TransferDst for the copy the caller records next.
        cmd.PrepareForAccess(m_CubeView, AccessKind::Sample);
    }

    vector<u8> SkyCubemapBake::BakeAndDownload(const MaterialInstance& material)
    {
        const u32 readbackSize = GetShReadbackFaceSize();
        const usize faceBytes = static_cast<usize>(readbackSize) * readbackSize * 8; // RGBA16F
        const Ref<Buffer> staging = Buffer::Create(m_Context, {
                                                                  .Name = "Sky Bake SH Readback",
                                                                  .Size = faceBytes * CubeFaces,
                                                                  .Usage = BufferUsage::TransferDst,
                                                              });

        // A self-contained submit: bake the six faces, reduce them to the readback level, copy that
        // level's six layers into the staging buffer (layer-major), and restore the sampled layout —
        // so the download reads the radiance this submit just produced, independent of the frame
        // command buffer. Blocks (ImmediateCommands submits + waits), bounded by the cold-start gate.
        m_Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                Bake(cmd, material);
                RecordReductionMips(cmd);
                cmd.PrepareForAccess(m_MipViews[m_ShReadbackMip], AccessKind::TransferSrc);
                const vk::BufferImageCopy region{
                    .bufferOffset = 0,
                    .bufferRowLength = 0,
                    .bufferImageHeight = 0,
                    .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                         .mipLevel = m_ShReadbackMip,
                                         .baseArrayLayer = 0,
                                         .layerCount = CubeFaces},
                    .imageOffset = {.x = 0, .y = 0, .z = 0},
                    .imageExtent = {.width = readbackSize, .height = readbackSize, .depth = 1},
                };
                GetVkCommandBuffer(cmd).copyImageToBuffer(GetVkImage(*m_CubeImage),
                                                          vk::ImageLayout::eTransferSrcOptimal,
                                                          GetVkBuffer(*staging), 1, &region);
                cmd.PrepareForAccess(m_MipViews[m_ShReadbackMip], AccessKind::Sample);
            });

        return staging->Download();
    }

    void SkyCubemapBake::BakeAtmosphere(CommandBuffer& cmd, const Ref<GraphicsPipeline>& pipeline,
                                        const Ref<DescriptorSet>& atmosphereSet,
                                        const Atmosphere& atmosphere, const vec3& sunDirection,
                                        const f32 intensity)
    {
        BindlessRegistry& registry = m_Context.GetBindlessRegistry();

        // Render each face into its single-layer view with the face's fixed basis in the view
        // constants and the far-plane stand-in in the depth slot. Each face claims its own view slot
        // so the six face constants coexist; the atmosphere fragment reconstructs the ray from the
        // slot the face pushes and samples the LUT set bound at set 1. The caller reserved the whole
        // cube's slots, so a refused claim here is a broken precondition, not a budget to degrade
        // against.
        for (u32 face = 0; face < CubeFaces; ++face)
        {
            const bool claimed = registry.TryBeginView();
            VE_ASSERT(claimed,
                      "SkyCubemapBake::BakeAtmosphere: the frame's view budget does not cover {} "
                      "cube faces; the caller must reserve them before recording a bake",
                      CubeFaces);
            ViewConstantsRegion region{};
            region.InvViewProj = m_FaceInvViewProj[face];
            // The face basis is already a pure direction mapping with no camera translation in it,
            // so it is its own rotation-only inverse and serves SkyViewDirection unchanged.
            region.InvViewRotProj = m_FaceInvViewProj[face];
            region.CameraPosition = vec4(0.0f);
            region.RenderScaleUV = vec4(1.0f, 1.0f, 1.0f, 1.0f);
            region.MaxValidUV = vec4(1.0f, 1.0f, 1.0f, 1.0f);
            registry.WriteViewConstants(std::as_bytes(std::span(&region, 1)));
            const u32 viewIndex = registry.GetCurrentViewConstantsIndex();

            cmd.PrepareForAccess(m_FaceViews[face], AccessKind::ColorAttachment);
            cmd.BeginRendering({
                .Extent = {m_FaceSize, m_FaceSize},
                .ColorAttachments = {{
                    .ImageView = m_FaceViews[face],
                    .LoadOp = LoadOp::Clear,
                    .StoreOp = StoreOp::Store,
                    .ClearValue = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
                }},
            });
            cmd.BindPipeline(pipeline);
            cmd.SetViewport({0, 0}, {m_FaceSize, m_FaceSize});
            cmd.SetScissor({0, 0}, {m_FaceSize, m_FaceSize});
            registry.Bind(cmd);
            cmd.BindDescriptorSets(DescriptorSetBindInfo{
                .Sets = {atmosphereSet},
                .FirstSet = 1,
                .PipelineBindPoint = PipelineBindPoint::Graphics,
            });
            cmd.PushConstants(AtmosphereSkyPushConstants{
                .DepthTexture = m_DepthHandle.Index,
                .Sampler = m_DepthSamplerHandle.Index,
                .ViewConstantsIndex = viewIndex,
                .Enabled = 1u,
                .SunDirection = sunDirection,
                .Intensity = intensity,
                .RayleighScattering = atmosphere.RayleighScattering,
                .RayleighHeight = atmosphere.RayleighHeight,
                .MieScattering = atmosphere.MieScattering,
                .MieExtinction = atmosphere.MieExtinction,
                .OzoneAbsorption = atmosphere.OzoneAbsorption,
                .MieHeight = atmosphere.MieHeight,
                .MieAnisotropy = atmosphere.MieAnisotropy,
                .OzoneCenter = atmosphere.OzoneCenter,
                .OzoneWidth = atmosphere.OzoneWidth,
                .PlanetRadius = atmosphere.PlanetRadius,
                .AtmosphereRadius = atmosphere.AtmosphereRadius,
                .SunAngularRadius = atmosphere.SunAngularRadius,
                .SunIrradiance = atmosphere.SunIrradiance,
            });
            cmd.DrawFullscreenTriangle();
            cmd.EndRendering();
        }
        m_FaceRendersRecorded += CubeFaces;

        // Leave the whole cube in a sampled layout for the skybox pass that samples it this frame.
        cmd.PrepareForAccess(m_CubeView, AccessKind::Sample);
    }

    vector<u8> SkyCubemapBake::BakeAtmosphereAndDownload(const Ref<GraphicsPipeline>& pipeline,
                                                         const Ref<DescriptorSet>& atmosphereSet,
                                                         const Atmosphere& atmosphere,
                                                         const vec3& sunDirection,
                                                         const f32 intensity)
    {
        const u32 readbackSize = GetShReadbackFaceSize();
        const usize faceBytes = static_cast<usize>(readbackSize) * readbackSize * 8; // RGBA16F
        const Ref<Buffer> staging = Buffer::Create(m_Context, {
                                                                  .Name = "Sky Bake SH Readback",
                                                                  .Size = faceBytes * CubeFaces,
                                                                  .Usage = BufferUsage::TransferDst,
                                                              });

        // A self-contained submit mirroring BakeAndDownload's material path: bake the six atmosphere
        // faces, reduce them to the readback level, copy that level's six layers (layer-major), and
        // restore the sampled layout — so the download reads the radiance this submit just produced.
        m_Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                BakeAtmosphere(cmd, pipeline, atmosphereSet, atmosphere, sunDirection, intensity);
                RecordReductionMips(cmd);
                cmd.PrepareForAccess(m_MipViews[m_ShReadbackMip], AccessKind::TransferSrc);
                const vk::BufferImageCopy region{
                    .bufferOffset = 0,
                    .bufferRowLength = 0,
                    .bufferImageHeight = 0,
                    .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                         .mipLevel = m_ShReadbackMip,
                                         .baseArrayLayer = 0,
                                         .layerCount = CubeFaces},
                    .imageOffset = {.x = 0, .y = 0, .z = 0},
                    .imageExtent = {.width = readbackSize, .height = readbackSize, .depth = 1},
                };
                GetVkCommandBuffer(cmd).copyImageToBuffer(GetVkImage(*m_CubeImage),
                                                          vk::ImageLayout::eTransferSrcOptimal,
                                                          GetVkBuffer(*staging), 1, &region);
                cmd.PrepareForAccess(m_MipViews[m_ShReadbackMip], AccessKind::Sample);
            });

        return staging->Download();
    }

    void SkyCubemapBake::RecordScratchFace(CommandBuffer& cmd, const MaterialInstance& material,
                                           const u32 face)
    {
        BindlessRegistry& registry = m_Context.GetBindlessRegistry();
        const u32 selector = material.GetMaterialSelector();

        // One face's worth of Bake, into the scratch cube: claim a view slot for the face basis and
        // draw the material fragment through it. Recorded from a service tick at the top of the
        // frame, where the view budget is freshly reset, so the single claim is affordable.
        const bool claimed = registry.TryBeginView();
        VE_ASSERT(claimed,
                  "SkyCubemapBake::RecordScratchFace: the frame's view budget is spent; an "
                  "amortized bake claims one view slot per tick");
        ViewConstantsRegion region{};
        region.InvViewProj = m_FaceInvViewProj[face];
        region.InvViewRotProj = m_FaceInvViewProj[face];
        region.CameraPosition = vec4(0.0f);
        region.RenderScaleUV = vec4(1.0f, 1.0f, 1.0f, 1.0f);
        region.MaxValidUV = vec4(1.0f, 1.0f, 1.0f, 1.0f);
        registry.WriteViewConstants(std::as_bytes(std::span(&region, 1)));
        const u32 viewIndex = registry.GetCurrentViewConstantsIndex();

        cmd.PrepareForAccess(m_ScratchFaceViews[face], AccessKind::ColorAttachment);
        cmd.BeginRendering({
            .Extent = {m_FaceSize, m_FaceSize},
            .ColorAttachments = {{
                .ImageView = m_ScratchFaceViews[face],
                .LoadOp = LoadOp::Clear,
                .StoreOp = StoreOp::Store,
                .ClearValue = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            }},
        });
        cmd.BindPipeline(m_Pipeline);
        cmd.SetViewport({0, 0}, {m_FaceSize, m_FaceSize});
        cmd.SetScissor({0, 0}, {m_FaceSize, m_FaceSize});
        registry.Bind(cmd);
        cmd.PushConstants(SkyMaterialPushConstants{
            .MaterialIndex = selector,
            .DepthTexture = m_DepthHandle.Index,
            .DepthSampler = m_DepthSamplerHandle.Index,
            .ViewConstantsIndex = viewIndex,
        });
        cmd.DrawFullscreenTriangle();
        cmd.EndRendering();
        ++m_FaceRendersRecorded;
    }

    void SkyCubemapBake::RecordScratchAtmosphereFace(CommandBuffer& cmd,
                                                     const Ref<GraphicsPipeline>& pipeline,
                                                     const Ref<DescriptorSet>& atmosphereSet,
                                                     const Atmosphere& atmosphere,
                                                     const vec3& sunDirection, const f32 intensity,
                                                     const u32 face)
    {
        BindlessRegistry& registry = m_Context.GetBindlessRegistry();

        const bool claimed = registry.TryBeginView();
        VE_ASSERT(claimed,
                  "SkyCubemapBake::RecordScratchAtmosphereFace: the frame's view budget is spent; "
                  "an amortized bake claims one view slot per tick");
        ViewConstantsRegion region{};
        region.InvViewProj = m_FaceInvViewProj[face];
        region.InvViewRotProj = m_FaceInvViewProj[face];
        region.CameraPosition = vec4(0.0f);
        region.RenderScaleUV = vec4(1.0f, 1.0f, 1.0f, 1.0f);
        region.MaxValidUV = vec4(1.0f, 1.0f, 1.0f, 1.0f);
        registry.WriteViewConstants(std::as_bytes(std::span(&region, 1)));
        const u32 viewIndex = registry.GetCurrentViewConstantsIndex();

        cmd.PrepareForAccess(m_ScratchFaceViews[face], AccessKind::ColorAttachment);
        cmd.BeginRendering({
            .Extent = {m_FaceSize, m_FaceSize},
            .ColorAttachments = {{
                .ImageView = m_ScratchFaceViews[face],
                .LoadOp = LoadOp::Clear,
                .StoreOp = StoreOp::Store,
                .ClearValue = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            }},
        });
        cmd.BindPipeline(pipeline);
        cmd.SetViewport({0, 0}, {m_FaceSize, m_FaceSize});
        cmd.SetScissor({0, 0}, {m_FaceSize, m_FaceSize});
        registry.Bind(cmd);
        cmd.BindDescriptorSets(DescriptorSetBindInfo{
            .Sets = {atmosphereSet},
            .FirstSet = 1,
            .PipelineBindPoint = PipelineBindPoint::Graphics,
        });
        cmd.PushConstants(AtmosphereSkyPushConstants{
            .DepthTexture = m_DepthHandle.Index,
            .Sampler = m_DepthSamplerHandle.Index,
            .ViewConstantsIndex = viewIndex,
            .Enabled = 1u,
            .SunDirection = sunDirection,
            .Intensity = intensity,
            .RayleighScattering = atmosphere.RayleighScattering,
            .RayleighHeight = atmosphere.RayleighHeight,
            .MieScattering = atmosphere.MieScattering,
            .MieExtinction = atmosphere.MieExtinction,
            .OzoneAbsorption = atmosphere.OzoneAbsorption,
            .MieHeight = atmosphere.MieHeight,
            .MieAnisotropy = atmosphere.MieAnisotropy,
            .OzoneCenter = atmosphere.OzoneCenter,
            .OzoneWidth = atmosphere.OzoneWidth,
            .PlanetRadius = atmosphere.PlanetRadius,
            .AtmosphereRadius = atmosphere.AtmosphereRadius,
            .SunAngularRadius = atmosphere.SunAngularRadius,
            .SunIrradiance = atmosphere.SunIrradiance,
        });
        cmd.DrawFullscreenTriangle();
        cmd.EndRendering();
        ++m_FaceRendersRecorded;
    }

    void SkyCubemapBake::AbandonBake()
    {
        CancelBake();
    }

    void SkyCubemapBake::CancelBake()
    {
        if (m_BakeState != BakeState::Idle)
        {
            m_Context.GetGeneratedTextures().Cancel(m_JobKey);
            m_BakeState = BakeState::Idle;
        }
    }

    void SkyCubemapBake::RequestBake(GeneratedTextureService& service,
                                     const MaterialInstance& material)
    {
        EnsurePipeline(material);

        // Supersede whatever bake is outstanding: a fresh dirty signal wants the newest content in
        // the scratch cube, not a completed or half-filled older one.
        CancelBake();

        const MaterialInstance* materialPtr = &material;
        GeneratedTextureRequest request{
            .Key = m_JobKey,
            .Name = "Sky Bake",
            .Targets = {{
                .Adopt = m_ScratchImage,
                .ProducerAccess = AccessKind::ColorAttachment,
                .SampledViewType = ImageViewType::Cube,
            }},
            .TickCount = CubeFaces,
            .OnTick =
                [this, materialPtr](CommandBuffer& cmd, const GeneratedTextureTickContext& context)
            { RecordScratchFace(cmd, *materialPtr, context.TickIndex); },
            .OnComplete = [this](const GeneratedTextureResult&)
            { m_BakeState = BakeState::Landed; },
        };
        service.Request(std::move(request));
        m_BakeState = BakeState::Pending;
    }

    void SkyCubemapBake::RequestBakeAtmosphere(GeneratedTextureService& service,
                                               const Ref<GraphicsPipeline>& pipeline,
                                               const Ref<DescriptorSet>& atmosphereSet,
                                               const Atmosphere& atmosphere,
                                               const vec3& sunDirection, const f32 intensity)
    {
        CancelBake();

        GeneratedTextureRequest request{
            .Key = m_JobKey,
            .Name = "Sky Bake Atmosphere",
            .Targets = {{
                .Adopt = m_ScratchImage,
                .ProducerAccess = AccessKind::ColorAttachment,
                .SampledViewType = ImageViewType::Cube,
            }},
            .TickCount = CubeFaces,
            .OnTick =
                [this, pipeline, atmosphereSet, atmosphere, sunDirection,
                 intensity](CommandBuffer& cmd, const GeneratedTextureTickContext& context)
            {
                RecordScratchAtmosphereFace(cmd, pipeline, atmosphereSet, atmosphere, sunDirection,
                                            intensity, context.TickIndex);
            },
            .OnComplete = [this](const GeneratedTextureResult&)
            { m_BakeState = BakeState::Landed; },
        };
        service.Request(std::move(request));
        m_BakeState = BakeState::Pending;
    }

    bool SkyCubemapBake::RecordAmortized(CommandBuffer& cmd)
    {
        if (m_BakeState != BakeState::Landed)
        {
            return false;
        }

        // The scratch cube holds a complete bake. Copy its six faces into the displayed cube's mip 0
        // in one step — an atomic swap of content, so the skybox never samples a half-filled cube —
        // and leave the displayed cube sampled for this frame's skybox pass. The reduction chain the
        // SH readback needs is regenerated from the fresh mip 0 by the caller's RecordReductionMips.
        cmd.PrepareForAccess(m_ScratchView, AccessKind::TransferSrc);
        cmd.PrepareForAccess(m_MipViews[0], AccessKind::TransferDst);
        const vk::ImageCopy copy{
            .srcSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                               .mipLevel = 0,
                               .baseArrayLayer = 0,
                               .layerCount = CubeFaces},
            .srcOffset = {.x = 0, .y = 0, .z = 0},
            .dstSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                               .mipLevel = 0,
                               .baseArrayLayer = 0,
                               .layerCount = CubeFaces},
            .dstOffset = {.x = 0, .y = 0, .z = 0},
            .extent = {.width = m_FaceSize, .height = m_FaceSize, .depth = 1},
        };
        GetVkCommandBuffer(cmd).copyImage(
            GetVkImage(*m_ScratchImage), vk::ImageLayout::eTransferSrcOptimal,
            GetVkImage(*m_CubeImage), vk::ImageLayout::eTransferDstOptimal, 1, &copy);
        cmd.PrepareForAccess(m_CubeView, AccessKind::Sample);

        // Drop the completed job (its adopted target is this bake's own scratch image, which stays)
        // so the key is free for the next re-bake.
        m_Context.GetGeneratedTextures().Release(m_JobKey);
        m_BakeState = BakeState::Idle;
        return true;
    }
}
