#include <Veng/Renderer/BakedSkyCube.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#include <fmt/format.h>

#include <Veng/Assert.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Renderer/BindlessRegistry.h>
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

        // A process-unique key per bake instance, so two BakedSkyCubes sharing one context's
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
            while (size > BakedSkyCube::ShReadbackFaceSize && (size % 2 == 0))
            {
                size /= 2;
                ++mip;
            }
            return mip;
        }

        // Edge length in texels of one amortization tile. Each amortized bake tick renders one tile
        // of a face through a scissor-clipped fullscreen draw, so the per-tick (and thus per-frame)
        // GPU cost is bounded by tile area rather than face area — a heavy per-texel sky fragment (a
        // volumetric ray-march) over a full 1024² face is otherwise one enormous dispatch. Smaller
        // tiles bound the per-frame cost tighter but take more frames for a re-baked sky to appear
        // (the previous cube stands meanwhile); 256 — 16 tiles per 1024² face — is the balance.
        constexpr u32 SkyBakeTilePixels = 256;

        // The reduced-resolution base layer (SkyBakeLayer::FaceSizeDivisor > 1) exists because its
        // per-pixel fragment is expensive — a volumetric sky march, say — so its coarse bake amortizes
        // at a finer tile than the detail layers: a quarter the edge, a sixteenth the area, so one bake
        // tick's fragment work stays bounded and no single frame runs a whole heavy face-tile. The
        // baked result is identical; only the number and size of the ticks it spreads across changes.
        constexpr u32 SkyBakeCoarseTilePixels = SkyBakeTilePixels / 4;

        // Tiles along one face axis at the given face size: ceil(faceSize / tile), so the last tile
        // is clamped when the face is not a whole multiple of the tile.
        constexpr u32 TilesPerFaceAxis(const u32 faceSize, const u32 tilePixels = SkyBakeTilePixels)
        {
            return (faceSize + tilePixels - 1) / tilePixels;
        }

        // A tile's pixel rect within a face: its top-left offset and its extent, the extent clamped
        // to the face so the last row/column tile of a non-multiple face covers only real texels.
        struct TileRect
        {
            uvec2 Offset;
            uvec2 Extent;
        };

        TileRect TileRectFor(const u32 tile, const u32 tilesPerAxis, const u32 faceSize,
                             const u32 tilePixels = SkyBakeTilePixels)
        {
            const u32 tileX = tile % tilesPerAxis;
            const u32 tileY = tile / tilesPerAxis;
            const uvec2 offset{tileX * tilePixels, tileY * tilePixels};
            return {
                .Offset = offset,
                .Extent = {std::min(tilePixels, faceSize - offset.x),
                           std::min(tilePixels, faceSize - offset.y)},
            };
        }

        // Field-wise blend-state equality, so a layered bake reuses a cached pipeline when its layer's
        // blend is unchanged. BlendState is a plain aggregate with no operator==.
        bool BlendEqual(const BlendState& a, const BlendState& b)
        {
            return a.Enable == b.Enable && a.SrcColorFactor == b.SrcColorFactor &&
                   a.DstColorFactor == b.DstColorFactor && a.ColorOp == b.ColorOp &&
                   a.SrcAlphaFactor == b.SrcAlphaFactor && a.DstAlphaFactor == b.DstAlphaFactor &&
                   a.AlphaOp == b.AlphaOp;
        }

        std::array<mat4, BakedSkyCube::CubeFaces> BuildFaceMatrices()
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

    Unique<BakedSkyCube> BakedSkyCube::Create(Context& context,
                                              const Ref<DescriptorSetLayout>& consumerLayout,
                                              const Format sceneColorFormat, const u32 faceSize)
    {
        return Unique<BakedSkyCube>(
            new BakedSkyCube(context, consumerLayout, sceneColorFormat, faceSize));
    }

    BakedSkyCube::BakedSkyCube(Context& context, const Ref<DescriptorSetLayout>& consumerLayout,
                               const Format sceneColorFormat, const u32 faceSize)
        : m_Context(context), m_SceneColorFormat(sceneColorFormat), m_FaceSize(faceSize),
          m_ShReadbackMip(ComputeShReadbackMip(faceSize)),
          m_TilesPerAxis(TilesPerFaceAxis(faceSize)),
          m_TilesPerFace(m_TilesPerAxis * m_TilesPerAxis), m_FaceInvViewProj(BuildFaceMatrices())
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
        // it needs no mips of its own. TransferSrc for that copy; TransferDst so a reduced-resolution
        // base layer's coarse cube can be upsampled into it by a blit; the service ORs in the rest.
        m_ScratchImage = Image::Create(
            m_Context, {
                           .Name = "Sky Bake Scratch Cube",
                           .Extent = {m_FaceSize, m_FaceSize, 1},
                           .MipLevels = 1,
                           .Layers = CubeFaces,
                           .Format = m_SceneColorFormat,
                           .Usage = ImageUsage::Sampled | ImageUsage::ColorAttachment |
                                    ImageUsage::TransferSrc | ImageUsage::TransferDst,
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

        // A 1×1 stand-in depth image holding the far-plane value (reverse-Z: 0.0), bound in the
        // fragment's depth slot so SkyIsBackground passes for every baked pixel — there is no
        // g-buffer at bake time. Sampled as a color texture (the fragment reads .r), so it is an
        // R32Sfloat sampled image.
        m_DepthImage =
            Image::Create(m_Context, {
                                         .Name = "Sky Bake Stand-in Depth",
                                         .Extent = {1, 1, 1},
                                         .Format = Format::R32Sfloat,
                                         .Usage = ImageUsage::Sampled | ImageUsage::TransferDst,
                                     });
        constexpr f32 farPlane = 0.0f;
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
        // frames later, and before that there is nothing baked to show.
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
                cmd.PrepareForAccess(m_CubeView, AccessKind::SampleAny);
            });
    }

    BakedSkyCube::~BakedSkyCube()
    {
        // Tear down any amortized bake still in flight so its tick/completion callbacks — which
        // capture this — never fire after it is gone.
        CancelBake();
        m_Context.GetBindlessRegistry().Release(m_DepthHandle);
    }

    Ref<GraphicsPipeline> BakedSkyCube::CreateBakePipeline(const MaterialInstance& material,
                                                           const BlendState& blend)
    {
        VE_ASSERT(material.GetDomain() == MaterialDomain::Sky,
                  "BakedSkyCube: material '{}' is not a Sky material", material.GetName());

        // The material's own fragment + the fullscreen vertex, against the cube-face color format with
        // the layer's blend on the one attachment. The layout (set 0 reserved, the sky push range)
        // comes from the material loader, so the fragment binds and pushes exactly as in the direct
        // path — it is unchanged and unaware of the bake.
        return GraphicsPipeline::Create(
            m_Context,
            {
                .Name = fmt::format("Sky Bake Pipeline ({})", material.GetName()),
                .ColorAttachments = {{.Format = m_SceneColorFormat, .Blend = blend}},
                .PipelineLayout = material.GetPipelineLayout(),
                .ShaderStages =
                    {
                        {.Stage = ShaderStage::Vertex, .Module = material.GetVertexModule()},
                        {.Stage = ShaderStage::Fragment, .Module = material.GetFragmentModule()},
                    },
            });
    }

    void BakedSkyCube::EnsurePipeline(const MaterialInstance& material)
    {
        // The bake pipeline depends only on the material's shader modules, its layout, and the fixed
        // cube-face format — never the instance's params, which ride the push range and the bindless
        // block — so every instance of one Sky material shares it. Key the cache on the fragment
        // module: a second instance of the same material (another world's copy) then reuses the
        // pipeline instead of recompiling the sky shader, which the per-instance index forced.
        if (m_Pipeline && m_PipelineFragment == material.GetFragmentModule().get())
        {
            return;
        }

        m_Pipeline = CreateBakePipeline(material, BlendState::Opaque());
        m_PipelineFragment = material.GetFragmentModule().get();
    }

    void BakedSkyCube::EnsureLayerPipelines(const std::span<const SkyBakeLayer> layers)
    {
        m_LayerPipelines.resize(layers.size());
        for (usize i = 0; i < layers.size(); ++i)
        {
            const SkyBakeLayer& layer = layers[i];
            VE_ASSERT(layer.Material != nullptr, "BakedSkyCube: bake layer {} has no material", i);
            const ShaderModule* const fragment = layer.Material->GetFragmentModule().get();
            LayerPipeline& slot = m_LayerPipelines[i];
            // Reuse the slot's pipeline when the layer's fragment and blend are unchanged — a re-bake
            // of the same stack recompiles no sky shader, the same reuse EnsurePipeline gives the
            // single-material path.
            if (slot.Pipeline && slot.Fragment == fragment && BlendEqual(slot.Blend, layer.Blend))
            {
                continue;
            }
            slot.Pipeline = CreateBakePipeline(*layer.Material, layer.Blend);
            slot.Fragment = fragment;
            slot.Blend = layer.Blend;
        }
    }

    void BakedSkyCube::EnsureCoarseScratch(const u32 coarseFaceSize)
    {
        if (m_CoarseScratchImage && m_CoarseFaceSize == coarseFaceSize)
        {
            return;
        }
        // The first coarse bake, or a bake at a different divisor: (re)create the coarse cube through
        // the retire path, so a prior one the GPU may still be reading is dropped safely. Rendered into
        // (ColorAttachment) then upsampled out of (TransferSrc); never sampled, so no Sampled usage.
        m_CoarseFaceSize = coarseFaceSize;
        m_CoarseScratchImage = Image::Create(
            m_Context, {
                           .Name = "Sky Bake Coarse Scratch Cube",
                           .Extent = {coarseFaceSize, coarseFaceSize, 1},
                           .MipLevels = 1,
                           .Layers = CubeFaces,
                           .Format = m_SceneColorFormat,
                           .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
                       });
        m_CoarseScratchView =
            ImageView::Create(m_Context, {
                                             .Name = "Sky Bake Coarse Scratch Cube View",
                                             .Image = m_CoarseScratchImage,
                                             .ViewType = ImageViewType::Array2D,
                                             .ArrayLayers = CubeFaces,
                                         });
        for (u32 face = 0; face < CubeFaces; ++face)
        {
            m_CoarseScratchFaceViews[face] = ImageView::Create(
                m_Context, {
                               .Name = fmt::format("Sky Bake Coarse Scratch Face {} View", face),
                               .Image = m_CoarseScratchImage,
                               .ViewType = ImageViewType::Type2D,
                               .BaseArrayLayer = face,
                               .ArrayLayers = 1,
                           });
        }
    }

    void BakedSkyCube::RecordCoarsePromote(CommandBuffer& cmd)
    {
        // Upsample all six coarse faces into the full-resolution scratch in one blit with a linear
        // filter, replacing their contents — the base the finer layers Load and blend over. Whole-image
        // transitions match the service's per-tick whole-image producer-access transition, so the
        // promote does not fight it face-by-face; one blit over all layers is the RecordReductionMips
        // pattern.
        cmd.PrepareForAccess(m_CoarseScratchView, AccessKind::TransferSrc);
        cmd.PrepareForAccess(m_ScratchView, AccessKind::TransferDst);
        const vk::ImageBlit blit{
            .srcSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                               .mipLevel = 0,
                               .baseArrayLayer = 0,
                               .layerCount = CubeFaces},
            .srcOffsets = {{vk::Offset3D{.x = 0, .y = 0, .z = 0},
                            vk::Offset3D{.x = static_cast<i32>(m_CoarseFaceSize),
                                         .y = static_cast<i32>(m_CoarseFaceSize),
                                         .z = 1}}},
            .dstSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                               .mipLevel = 0,
                               .baseArrayLayer = 0,
                               .layerCount = CubeFaces},
            .dstOffsets = {{vk::Offset3D{.x = 0, .y = 0, .z = 0},
                            vk::Offset3D{.x = static_cast<i32>(m_FaceSize),
                                         .y = static_cast<i32>(m_FaceSize),
                                         .z = 1}}},
        };
        GetVkCommandBuffer(cmd).blitImage(
            GetVkImage(*m_CoarseScratchImage), vk::ImageLayout::eTransferSrcOptimal,
            GetVkImage(*m_ScratchImage), vk::ImageLayout::eTransferDstOptimal, 1, &blit,
            vk::Filter::eLinear);
    }

    u32 BakedSkyCube::AcquireFaceViewSlot(CommandBuffer& cmd, const u32 face, const bool faceFirst)
    {
        BindlessRegistry& registry = m_Context.GetBindlessRegistry();

        // One view slot per face, reused by that face's tiles within a single recording (one frame's
        // command buffer). Every tile of a face writes the identical face basis, so one slot serves
        // them all — bounding a whole-cube bake to CubeFaces view slots per frame regardless of how
        // many tiles it renders or how many run in one pump (an UnlimitedTickBudget re-bake of a
        // large face would otherwise claim one slot per tile, far past MaxViewsPerFrame). A fresh
        // slot is claimed at the face's first tile and whenever a new frame's command buffer starts
        // — an amortized face split across frames re-claims each frame, since a slot claimed in a
        // prior frame is not this frame's to reference. Frames-in-flight is >= 2, so consecutive
        // frames never share a command buffer and the reuse branch is never taken across a frame
        // boundary.
        if (faceFirst || &cmd != m_LastTileCmd || face != m_LastTileFace)
        {
            const bool claimed = registry.TryBeginView();
            VE_ASSERT(claimed,
                      "BakedSkyCube: the frame's view budget is spent; a bake claims one view "
                      "slot per face and the caller must reserve them");
            ViewConstantsRegion region{};
            region.InvViewProj = m_FaceInvViewProj[face];
            // The face basis is already a pure direction mapping with no camera translation in it, so
            // it is its own rotation-only inverse and serves SkyViewDirection unchanged.
            region.InvViewRotProj = m_FaceInvViewProj[face];
            region.CameraPosition = vec4(0.0f);
            region.RenderScaleUV = vec4(1.0f, 1.0f, 1.0f, 1.0f);
            region.MaxValidUV = vec4(1.0f, 1.0f, 1.0f, 1.0f);
            registry.WriteViewConstants(std::as_bytes(std::span(&region, 1)));
            m_LastTileViewIndex = registry.GetCurrentViewConstantsIndex();
            m_LastTileCmd = &cmd;
            m_LastTileFace = face;
        }
        return m_LastTileViewIndex;
    }

    void BakedSkyCube::RecordMaterialRegion(CommandBuffer& cmd,
                                            const Ref<GraphicsPipeline>& pipeline,
                                            const MaterialInstance& material,
                                            const Ref<ImageView>& faceView, const u32 faceSize,
                                            const u32 face, const uvec2 tileOffset,
                                            const uvec2 tileExtent, const bool clear)
    {
        const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
        const u32 selector = material.GetMaterialSelector();

        // Draw the material fragment through the face's fixed basis, with the far-plane stand-in in
        // the depth slot so SkyIsBackground passes. A fresh view slot is claimed at the face's first
        // tile (offset 0,0) — which under a layered bake is not the layer that clears, so the claim is
        // keyed on the tile, not the clear.
        const bool faceFirst = tileOffset.x == 0 && tileOffset.y == 0;
        const u32 viewIndex = AcquireFaceViewSlot(cmd, face, faceFirst);

        cmd.PrepareForAccess(faceView, AccessKind::ColorAttachment);
        // Render-area = the tile, so a Clear touches only this tile (a whole-face Clear would erase
        // tiles already baked into this face); Clear the face's first tile, Load every later one.
        cmd.BeginRendering({
            .Offset = {static_cast<i32>(tileOffset.x), static_cast<i32>(tileOffset.y)},
            .Extent = tileExtent,
            .ColorAttachments = {{
                .ImageView = faceView,
                .LoadOp = clear ? LoadOp::Clear : LoadOp::Load,
                .StoreOp = StoreOp::Store,
                .ClearValue = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            }},
        });
        cmd.BindPipeline(pipeline);
        // Viewport spans the whole face so the SV_Position → UV → world-direction mapping is the
        // full-face one; the tile render-area + scissor clip the fullscreen triangle to this tile's
        // texels, each computing the same cube direction it would in a whole-face draw.
        cmd.SetViewport({0, 0}, {faceSize, faceSize});
        cmd.SetScissor({static_cast<i32>(tileOffset.x), static_cast<i32>(tileOffset.y)},
                       tileExtent);
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

    void BakedSkyCube::Bake(CommandBuffer& cmd, const MaterialInstance& material)
    {
        EnsurePipeline(material);
        // The synchronous path renders whole faces: one region draw per face covering it, Clear.
        for (u32 face = 0; face < CubeFaces; ++face)
        {
            RecordMaterialRegion(cmd, m_Pipeline, material, m_FaceViews[face], m_FaceSize, face,
                                 {0, 0}, {m_FaceSize, m_FaceSize}, true);
        }
        // Leave the whole cube sampled for this frame's readers: the skybox pass samples it in a
        // fragment, and the IBL convolution reads it with a dispatch.
        cmd.PrepareForAccess(m_CubeView, AccessKind::SampleAny);
    }

    void BakedSkyCube::Bake(CommandBuffer& cmd, const std::span<const SkyBakeLayer> layers)
    {
        EnsureLayerPipelines(layers);
        // A reduced-resolution base is amortized-path only: the promote is an extra scratch cube and a
        // blit that the whole-face synchronous path does not carry, and the sync path is a reference
        // convenience the amortized RequestBake is the shipping route past.
        for (const SkyBakeLayer& layer : layers)
        {
            VE_ASSERT(layer.FaceSizeDivisor <= 1,
                      "BakedSkyCube::Bake: FaceSizeDivisor > 1 needs the amortized RequestBake");
        }
        // The synchronous layered path renders whole faces, layer by layer: the first layer clears
        // each face, every later layer Loads and blends over it, so the finished cube is the
        // composite. Layer-major so a face's clear precedes every blend onto it.
        for (usize layer = 0; layer < layers.size(); ++layer)
        {
            for (u32 face = 0; face < CubeFaces; ++face)
            {
                RecordMaterialRegion(cmd, m_LayerPipelines[layer].Pipeline, *layers[layer].Material,
                                     m_FaceViews[face], m_FaceSize, face, {0, 0},
                                     {m_FaceSize, m_FaceSize}, layer == 0);
            }
        }
        // Leave the whole cube sampled for this frame's readers: the skybox pass samples it in a
        // fragment, and the IBL convolution reads it with a dispatch.
        cmd.PrepareForAccess(m_CubeView, AccessKind::SampleAny);
    }

    void BakedSkyCube::RecordReductionMips(CommandBuffer& cmd)
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

        // Restore mip 0 (the display cube) to a sampled layout for this frame's fragment and
        // dispatch readers; the readback level is left in TransferDst for the caller's copy.
        cmd.PrepareForAccess(m_CubeView, AccessKind::SampleAny);
    }

    void BakedSkyCube::RecordAtmosphereRegion(CommandBuffer& cmd,
                                              const Ref<GraphicsPipeline>& pipeline,
                                              const Ref<DescriptorSet>& atmosphereSet,
                                              const Atmosphere& atmosphere,
                                              const vec3& sunDirection, const f32 intensity,
                                              const Ref<ImageView>& faceView, const u32 faceSize,
                                              const u32 face, const uvec2 tileOffset,
                                              const uvec2 tileExtent, const bool clear)
    {
        const BindlessRegistry& registry = m_Context.GetBindlessRegistry();

        // Draw the atmosphere fragment through the face's fixed basis, sampling the LUT set bound at
        // set 3, with the far-plane stand-in in the depth slot.
        const u32 viewIndex = AcquireFaceViewSlot(cmd, face, clear);

        cmd.PrepareForAccess(faceView, AccessKind::ColorAttachment);
        // Render-area = the tile, so a Clear touches only this tile (a whole-face Clear would erase
        // tiles already baked into this face); Clear the face's first tile, Load every later one.
        cmd.BeginRendering({
            .Offset = {static_cast<i32>(tileOffset.x), static_cast<i32>(tileOffset.y)},
            .Extent = tileExtent,
            .ColorAttachments = {{
                .ImageView = faceView,
                .LoadOp = clear ? LoadOp::Clear : LoadOp::Load,
                .StoreOp = StoreOp::Store,
                .ClearValue = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            }},
        });
        cmd.BindPipeline(pipeline);
        // Viewport spans the whole face so the SV_Position → UV → world-direction mapping is the
        // full-face one; the tile render-area + scissor clip the fullscreen triangle to this tile.
        cmd.SetViewport({0, 0}, {faceSize, faceSize});
        cmd.SetScissor({static_cast<i32>(tileOffset.x), static_cast<i32>(tileOffset.y)},
                       tileExtent);
        registry.Bind(cmd);
        cmd.BindDescriptorSets(DescriptorSetBindInfo{
            .Sets = {atmosphereSet},
            .FirstSet = 3,
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

    void BakedSkyCube::BakeAtmosphere(CommandBuffer& cmd, const Ref<GraphicsPipeline>& pipeline,
                                      const Ref<DescriptorSet>& atmosphereSet,
                                      const Atmosphere& atmosphere, const vec3& sunDirection,
                                      const f32 intensity)
    {
        // The synchronous path renders whole faces: one region draw per face covering it, Clear.
        for (u32 face = 0; face < CubeFaces; ++face)
        {
            RecordAtmosphereRegion(cmd, pipeline, atmosphereSet, atmosphere, sunDirection,
                                   intensity, m_FaceViews[face], m_FaceSize, face, {0, 0},
                                   {m_FaceSize, m_FaceSize}, true);
        }
        // Leave the whole cube sampled for this frame's readers: the skybox pass samples it in a
        // fragment, and the IBL convolution reads it with a dispatch.
        cmd.PrepareForAccess(m_CubeView, AccessKind::SampleAny);
    }

    void BakedSkyCube::AbandonBake()
    {
        CancelBake();
    }

    void BakedSkyCube::CancelBake()
    {
        if (m_BakeState != BakeState::Idle)
        {
            m_Context.GetGeneratedTextures().Cancel(m_JobKey);
            m_BakeState = BakeState::Idle;
        }
    }

    void BakedSkyCube::RequestBake(GeneratedTextureService& service,
                                   const MaterialInstance& material)
    {
        // The single-material bake is one opaque layer that clears the cube — the layered path's
        // degenerate case.
        const SkyBakeLayer layer{.Material = &material, .Blend = BlendState::Opaque()};
        RequestBake(service, std::span<const SkyBakeLayer>(&layer, 1));
    }

    void BakedSkyCube::RequestBake(GeneratedTextureService& service,
                                   const std::span<const SkyBakeLayer> layers, string cacheKey)
    {
        VE_ASSERT(!layers.empty(), "BakedSkyCube::RequestBake: a bake needs at least one layer");
        EnsureLayerPipelines(layers);

        // Supersede whatever bake is outstanding: a fresh dirty signal wants the newest content in
        // the scratch cube, not a completed or half-filled older one.
        CancelBake();

        // Resolve each layer's (pipeline, material) and capture the list by value, so the tick lambda
        // is decoupled from m_LayerPipelines — a superseding request rebuilds that in place, but this
        // job (cancelled by the same request) holds its own resolved pipelines.
        struct LayerRender
        {
            Ref<GraphicsPipeline> Pipeline;
            const MaterialInstance* Material;
        };
        vector<LayerRender> render;
        render.reserve(layers.size());
        for (usize i = 0; i < layers.size(); ++i)
        {
            render.push_back({m_LayerPipelines[i].Pipeline, layers[i].Material});
        }
        const u32 perLayer = CubeFaces * m_TilesPerFace;

        // The base layer (layer 0, the opaque clear) may bake at a reduced resolution, upsampled into
        // the full cube before the finer layers composite over it; only it may, because the promote is
        // a replacing blit the later blended layers must Load over at full resolution.
        const u32 baseDivisor = std::max(layers[0].FaceSizeDivisor, 1u);
        VE_ASSERT(
            (baseDivisor & (baseDivisor - 1)) == 0 && m_FaceSize % baseDivisor == 0,
            "BakedSkyCube::RequestBake: base FaceSizeDivisor {} must be a power of two dividing "
            "the face size {}",
            baseDivisor, m_FaceSize);
        for (usize i = 1; i < layers.size(); ++i)
        {
            VE_ASSERT(layers[i].FaceSizeDivisor <= 1,
                      "BakedSkyCube::RequestBake: only the base layer may set FaceSizeDivisor > 1 "
                      "(layer {} set {})",
                      i, layers[i].FaceSizeDivisor);
        }

        // A full-resolution base renders straight into the scratch cube, layer-major: TickIndex
        // enumerates (layer, face, tile). A reduced base runs three phases — the base into the coarse
        // cube, one upsample-blit per face, then the finer layers full-res over the promoted base — so
        // its fragment count falls by the divisor squared while the detail layers stay sharp.
        u32 baseTicks = perLayer;
        u32 coarseFaceSize = m_FaceSize;
        u32 coarseTilesPerAxis = m_TilesPerAxis;
        u32 coarseTilesPerFace = m_TilesPerFace;
        u32 promoteTicks = 0;
        if (baseDivisor > 1)
        {
            coarseFaceSize = m_FaceSize / baseDivisor;
            EnsureCoarseScratch(coarseFaceSize);
            coarseTilesPerAxis = TilesPerFaceAxis(coarseFaceSize, SkyBakeCoarseTilePixels);
            coarseTilesPerFace = coarseTilesPerAxis * coarseTilesPerAxis;
            baseTicks = CubeFaces * coarseTilesPerFace;
            promoteTicks = 1;
        }
        const u32 tickCount = baseDivisor > 1 ? baseTicks + promoteTicks +
                                                    static_cast<u32>(layers.size() - 1) * perLayer
                                              : static_cast<u32>(layers.size()) * perLayer;

        GeneratedTextureRequest request{
            .Key = m_JobKey,
            .Name = "Sky Bake",
            .Targets = {{
                .Adopt = m_ScratchImage,
                .ProducerAccess = AccessKind::ColorAttachment,
                .SampledViewType = ImageViewType::Cube,
            }},
            .CacheKey = std::move(cacheKey),
            .TickCount = tickCount,
            .OnTick =
                [this, render = std::move(render), perLayer, baseDivisor, coarseFaceSize,
                 coarseTilesPerAxis, coarseTilesPerFace, baseTicks,
                 promoteTicks](CommandBuffer& cmd, const GeneratedTextureTickContext& context)
            {
                const u32 idx = context.TickIndex;
                if (baseDivisor <= 1)
                {
                    // One tile per tick, layer-major: every face of a layer is filled before the next
                    // blends over it. The first layer's first tile of each face clears; the rest Load.
                    const u32 layer = idx / perLayer;
                    const u32 within = idx % perLayer;
                    const u32 face = within / m_TilesPerFace;
                    const u32 tile = within % m_TilesPerFace;
                    const TileRect rect = TileRectFor(tile, m_TilesPerAxis, m_FaceSize);
                    RecordMaterialRegion(cmd, render[layer].Pipeline, *render[layer].Material,
                                         m_ScratchFaceViews[face], m_FaceSize, face, rect.Offset,
                                         rect.Extent, layer == 0 && tile == 0);
                    return;
                }
                // Phase 1: the base layer into the coarse cube (its own tile grid; first tile clears).
                if (idx < baseTicks)
                {
                    const u32 face = idx / coarseTilesPerFace;
                    const u32 tile = idx % coarseTilesPerFace;
                    const TileRect rect = TileRectFor(tile, coarseTilesPerAxis, coarseFaceSize,
                                                      SkyBakeCoarseTilePixels);
                    RecordMaterialRegion(cmd, render[0].Pipeline, *render[0].Material,
                                         m_CoarseScratchFaceViews[face], coarseFaceSize, face,
                                         rect.Offset, rect.Extent, tile == 0);
                    return;
                }
                // Phase 2: one blit upsamples the whole coarse cube into the full scratch as the base.
                if (idx < baseTicks + promoteTicks)
                {
                    RecordCoarsePromote(cmd);
                    return;
                }
                // Phase 3: the finer layers full-res over the promoted base, Loading and blending.
                const u32 j = idx - baseTicks - promoteTicks;
                const u32 layer = 1 + j / perLayer;
                const u32 within = j % perLayer;
                const u32 face = within / m_TilesPerFace;
                const u32 tile = within % m_TilesPerFace;
                const TileRect rect = TileRectFor(tile, m_TilesPerAxis, m_FaceSize);
                RecordMaterialRegion(cmd, render[layer].Pipeline, *render[layer].Material,
                                     m_ScratchFaceViews[face], m_FaceSize, face, rect.Offset,
                                     rect.Extent, false);
            },
            .OnComplete = [this](const GeneratedTextureResult&)
            { m_BakeState = BakeState::Landed; },
        };
        service.Request(std::move(request));
        m_BakeState = BakeState::Pending;
    }

    void BakedSkyCube::RequestBakeAtmosphere(GeneratedTextureService& service,
                                             const Ref<GraphicsPipeline>& pipeline,
                                             const Ref<DescriptorSet>& atmosphereSet,
                                             const Atmosphere& atmosphere, const vec3& sunDirection,
                                             const f32 intensity)
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
            .TickCount = CubeFaces * m_TilesPerFace,
            .OnTick =
                [this, pipeline, atmosphereSet, atmosphere, sunDirection,
                 intensity](CommandBuffer& cmd, const GeneratedTextureTickContext& context)
            {
                // One tile per tick: TickIndex enumerates (face, tile) in face-major order.
                const u32 face = context.TickIndex / m_TilesPerFace;
                const u32 tile = context.TickIndex % m_TilesPerFace;
                const TileRect rect = TileRectFor(tile, m_TilesPerAxis, m_FaceSize);
                RecordAtmosphereRegion(cmd, pipeline, atmosphereSet, atmosphere, sunDirection,
                                       intensity, m_ScratchFaceViews[face], m_FaceSize, face,
                                       rect.Offset, rect.Extent, tile == 0);
            },
            .OnComplete = [this](const GeneratedTextureResult&)
            { m_BakeState = BakeState::Landed; },
        };
        service.Request(std::move(request));
        m_BakeState = BakeState::Pending;
    }

    bool BakedSkyCube::RecordAmortized(CommandBuffer& cmd)
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
        cmd.PrepareForAccess(m_CubeView, AccessKind::SampleAny);

        // Drop the completed job (its adopted target is this bake's own scratch image, which stays)
        // so the key is free for the next re-bake.
        m_Context.GetGeneratedTextures().Release(m_JobKey);
        m_BakeState = BakeState::Idle;
        // A fresh bake is now displayed: advance the revision so every renderer sampling this cube
        // re-derives its IBL/SH from the new content (see GetRevision).
        ++m_Revision;
        return true;
    }

    Ref<DescriptorSetLayout> BakedSkyCube::CreateConsumerSetLayout(Context& context)
    {
        // The radiance/irradiance/prefilter cubes at 0/1/2, the BRDF LUT at 3, the linear sampler at
        // 4 — the image-based-lighting consumer set's shape. A baked cube writes only the radiance
        // (0) and the sampler (4); the skybox pipeline binds the whole layout, so a baked cube's set
        // and the IBL lighting set must share it. This is its one definition; EnvironmentIbl builds
        // its own consumer set against this, and a service that owns a cube without a SceneRenderer
        // creates a compatible layout here.
        return DescriptorSetLayout::Create(context,
                                           {
                                               .Name = "Sky Radiance Consumer Set Layout",
                                               .Bindings =
                                                   {
                                                       {.Binding = 0,
                                                        .Type = DescriptorType::SampledImage,
                                                        .Count = 1,
                                                        .Stages = ShaderStage::Fragment},
                                                       {.Binding = 1,
                                                        .Type = DescriptorType::SampledImage,
                                                        .Count = 1,
                                                        .Stages = ShaderStage::Fragment},
                                                       {.Binding = 2,
                                                        .Type = DescriptorType::SampledImage,
                                                        .Count = 1,
                                                        .Stages = ShaderStage::Fragment},
                                                       {.Binding = 3,
                                                        .Type = DescriptorType::SampledImage,
                                                        .Count = 1,
                                                        .Stages = ShaderStage::Fragment},
                                                       {.Binding = 4,
                                                        .Type = DescriptorType::Sampler,
                                                        .Count = 1,
                                                        .Stages = ShaderStage::Fragment},
                                                   },
                                           });
    }
}
