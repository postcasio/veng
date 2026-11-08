#include <Veng/Renderer/BakedSkyCube.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>

#include <fmt/format.h>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/GeneratedTextureService.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/Sampler.h>

#include "GpuBlocks.h"

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
            u32 MaterialOffset;
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

        static_assert(BakedSkyCube::MaxFieldTargets == MaxSkyFieldTargets,
                      "a field layer writes at most as many targets as a material may declare");

        // The solid-angle sum's compute shader (sky_bake_field_sum.comp).
        constexpr AssetId SkyBakeFieldSumCompId{0xA800B88B38A9D722ULL};

        // The solid-angle sum's push block, matching sky_bake_field_sum.comp PushConstants.
        struct FieldSumPush
        {
            u32 FaceSize;
            u32 OutputBase;
        };

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

        // The first mip of a chain whose face edge is at or below `limit`.
        u32 FirstMipAtOrBelow(const u32 faceSize, const u32 limit)
        {
            u32 mip = 0;
            while ((faceSize >> mip) > limit)
            {
                ++mip;
            }
            return mip;
        }

        // The encode's push block, matching sky_bake_bc6h.comp PushConstants.
        struct EncodePush
        {
            uvec2 SourceSize;
            uvec2 BlocksPerAxis;
            u32 OutputBase;
            u32 Pad0;
            u32 Pad1;
            u32 Pad2;
        };

        // The BC6H block encoder's compute shader (sky_bake_bc6h.comp).
        constexpr AssetId SkyBakeBc6hCompId{0x7014DD2D79F894ECULL};

        // Bytes of one BC6H block.
        constexpr u64 EncodeBlockBytes = 16;

        // The 4x4 blocks along one axis of an edge, a partial block counting as one.
        constexpr u32 BlocksAlong(const u32 texels)
        {
            return (texels + 3) / 4;
        }

        // A 2x2 box blit from one mip to the next over `layerCount` layers from `baseLayer`.
        void RecordHalvingBlit(CommandBuffer& cmd, const Ref<Image>& image, const u32 dstMip,
                               const u32 srcSize, const u32 baseLayer, const u32 layerCount)
        {
            const i32 size = static_cast<i32>(srcSize);
            const i32 half = static_cast<i32>(std::max(1u, srcSize / 2));
            cmd.BlitImage({
                .SourceImage = image,
                .DestinationImage = image,
                .SourceMipLevel = dstMip - 1,
                .DestinationMipLevel = dstMip,
                .SourceOffset = {0, 0, 0},
                .DestinationOffset = {0, 0, 0},
                .SourceExtent = {size, size, 1},
                .DestinationExtent = {half, half, 1},
                .SourceBaseLayer = baseLayer,
                .DestinationBaseLayer = baseLayer,
                .LayerCount = layerCount,
            });
        }

        // Copies every mip of every layer of one cube into another of the same shape and format.
        void RecordWholeChainCopy(CommandBuffer& cmd, const Ref<Image>& source,
                                  const Ref<Image>& destination)
        {
            const u32 faceSize = source->GetExtent().x;
            vector<ImageCopyRegion> regions;
            regions.reserve(source->GetMipLevels());
            for (u32 mip = 0; mip < source->GetMipLevels(); ++mip)
            {
                const u32 size = std::max(1u, faceSize >> mip);
                regions.push_back({
                    .SourceMipLevel = mip,
                    .DestinationMipLevel = mip,
                    .LayerCount = BakedSkyCube::CubeFaces,
                    .Extent = {size, size, 1},
                });
            }
            cmd.CopyImageRegions(source, destination, regions);
        }

        // The default edge length in texels of one amortization tile — the size a bake layer that
        // names no tile size of its own takes (SkyBakeLayer::TilePixels). Each amortized bake tick
        // renders one tile of a face through a scissor-clipped fullscreen draw, so the per-tick (and
        // thus per-frame) GPU cost is bounded by tile area rather than face area — a heavy per-texel
        // sky fragment (a volumetric ray-march) over a full 1024² face is otherwise one enormous
        // dispatch. Smaller tiles bound the per-frame cost tighter but take more frames for a re-baked
        // sky to appear (the previous cube stands meanwhile); 256 — 16 tiles per 1024² face — is the
        // balance a layer keeps unless its owner sets a tile size to bound one tick's work itself.
        constexpr u32 SkyBakeTilePixels = 256;

        // Tiles along one face axis at the given face size and tile size: ceil(faceSize / tile), so
        // the last tile is clamped when the face is not a whole multiple of the tile.
        constexpr u32 TilesPerFaceAxis(const u32 faceSize, const u32 tilePixels)
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
                             const u32 tilePixels)
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
                                              const Format sceneColorFormat, const u32 faceSize,
                                              AssetManager* assets, const Format displayFormat)
    {
        return Unique<BakedSkyCube>(new BakedSkyCube(context, consumerLayout, sceneColorFormat,
                                                     faceSize, assets, displayFormat));
    }

    BakedSkyCube::BakedSkyCube(Context& context, const Ref<DescriptorSetLayout>& consumerLayout,
                               const Format sceneColorFormat, const u32 faceSize,
                               AssetManager* assets, const Format displayFormat)
        : m_Context(context), m_SceneColorFormat(sceneColorFormat),
          m_DisplayFormat(displayFormat == Format::Undefined ? sceneColorFormat : displayFormat),
          m_FaceSize(faceSize), m_MipCount(static_cast<u32>(std::bit_width(faceSize))),
          m_TilesPerAxis(TilesPerFaceAxis(faceSize, SkyBakeTilePixels)),
          m_TilesPerFace(m_TilesPerAxis * m_TilesPerAxis), m_FaceInvViewProj(BuildFaceMatrices())
    {
        const bool compressed = IsCompressed();
        VE_ASSERT(m_DisplayFormat == m_SceneColorFormat || compressed,
                  "BakedSkyCube: the display format must be the scene-color format or BC6HUfloat");
        if (compressed)
        {
            VE_ASSERT(m_Context.IsBlockCompressionSupported(),
                      "BakedSkyCube: a BC6H cube needs a device with BC block compression");
            VE_ASSERT(assets != nullptr, "BakedSkyCube: a BC6H cube needs an AssetManager");
            VE_ASSERT(m_FaceSize >= 4 && std::has_single_bit(m_FaceSize),
                      "BakedSkyCube: a BC6H cube's face size {} must be a power of two of at "
                      "least 4",
                      m_FaceSize);
        }

        // The radiance cube: six layers sampled as a cube, with a full mip chain the skybox samples
        // trilinear. A float cube renders its faces at the scene-color format, so its radiance
        // round-trips the skybox sampler with no conversion, and rebuilds its chain by box blits; a
        // compressed one is written only by copies of the encoded chain.
        m_CubeImage = Image::Create(
            m_Context,
            {
                .Name = "Sky Bake Radiance Cube",
                .Extent = {m_FaceSize, m_FaceSize, 1},
                .MipLevels = m_MipCount,
                .Layers = CubeFaces,
                .Format = m_DisplayFormat,
                .Usage = compressed ? ImageUsage::Sampled | ImageUsage::TransferSrc |
                                          ImageUsage::TransferDst
                                    : ImageUsage::Sampled | ImageUsage::ColorAttachment |
                                          ImageUsage::TransferSrc | ImageUsage::TransferDst,
            });
        m_CubeView = ImageView::Create(m_Context, {
                                                      .Name = "Sky Bake Radiance Cube View",
                                                      .Image = m_CubeImage,
                                                      .ViewType = ImageViewType::Cube,
                                                      .MipLevels = m_MipCount,
                                                      .ArrayLayers = CubeFaces,
                                                  });
        for (u32 face = 0; face < CubeFaces && !compressed; ++face)
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
        // One all-six-layers view per mip level, for the chain's layout transitions (the raw blit
        // addresses subresources directly, but PrepareForAccess tracks through a view).
        m_MipViews.reserve(m_MipCount);
        for (u32 mip = 0; mip < m_MipCount; ++mip)
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

        // The lighting reduction of a float cube is its own mip tail from the first level at or
        // below LightingFaceSize, so a float cube that small lights from exactly what it displays.
        if (!compressed)
        {
            m_LightingImage = m_CubeImage;
            m_LightingBaseMip = FirstMipAtOrBelow(m_FaceSize, LightingFaceSize);
            m_LightingFaceSize = std::max(1u, m_FaceSize >> m_LightingBaseMip);
            m_LightingView =
                m_LightingBaseMip == 0
                    ? m_CubeView
                    : ImageView::Create(m_Context, {
                                                       .Name = "Sky Bake Lighting View",
                                                       .Image = m_CubeImage,
                                                       .ViewType = ImageViewType::Cube,
                                                       .BaseMipLevel = m_LightingBaseMip,
                                                       .MipLevels = m_MipCount - m_LightingBaseMip,
                                                       .ArrayLayers = CubeFaces,
                                                   });
            m_ShReadbackMip = FirstMipAtOrBelow(m_FaceSize, ShReadbackFaceSize);
            m_ShReadbackFaceSize = std::max(1u, m_FaceSize >> m_ShReadbackMip);
        }

        // The scratch cube an amortized bake fills. A float scratch is mip 0 only — the displayed
        // cube rebuilds its chain from it on landing — and is TransferDst so a reduced-resolution
        // base layer's coarse cube can be upsampled into it by a blit; a compressed scratch holds
        // the whole encoded chain, written by copies. The service ORs in the rest.
        m_ScratchImage = Image::Create(
            m_Context,
            {
                .Name = "Sky Bake Scratch Cube",
                .Extent = {m_FaceSize, m_FaceSize, 1},
                .MipLevels = compressed ? m_MipCount : 1,
                .Layers = CubeFaces,
                .Format = m_DisplayFormat,
                .Usage = compressed ? ImageUsage::Sampled | ImageUsage::TransferSrc |
                                          ImageUsage::TransferDst
                                    : ImageUsage::Sampled | ImageUsage::ColorAttachment |
                                          ImageUsage::TransferSrc | ImageUsage::TransferDst,
            });
        m_ScratchView =
            ImageView::Create(m_Context, {
                                             .Name = "Sky Bake Scratch Cube View",
                                             .Image = m_ScratchImage,
                                             .ViewType = ImageViewType::Array2D,
                                             .MipLevels = m_ScratchImage->GetMipLevels(),
                                             .ArrayLayers = CubeFaces,
                                         });
        for (u32 face = 0; face < CubeFaces && !compressed; ++face)
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

        // A cube that may bake a field loads the field's solid-angle sum and its storage up front,
        // so a field bake later needs nothing from the asset manager.
        if (assets != nullptr)
        {
            BindlessRegistry& registry = m_Context.GetBindlessRegistry();
            const AssetResult<AssetHandle<Veng::Shader>> sumCs =
                assets->LoadSync<Veng::Shader>(SkyBakeFieldSumCompId);
            VE_ASSERT(sumCs.has_value(), "BakedSkyCube: field sum shader load failed: {}",
                      sumCs.error().Detail);
            m_FieldSumSetLayout = DescriptorSetLayout::Create(
                m_Context, {
                               .Name = "Sky Bake Field Sum Set Layout",
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
                                   },
                           });
            const Ref<PipelineLayout> layout = PipelineLayout::Create(
                m_Context, {
                               .Name = "Sky Bake Field Sum Layout",
                               .DescriptorSetLayouts = {m_FieldSumSetLayout},
                               .PushConstantRanges = {PushConstantRange::Of<FieldSumPush>(
                                   ShaderStage::Compute)},
                           });
            m_FieldSumPipeline = ComputePipeline::Create(
                m_Context, {
                               .Name = "Sky Bake Field Sum Pipeline",
                               .PipelineLayout = layout,
                               .ShaderStage = {.Stage = ShaderStage::Compute,
                                               .Module = sumCs.value().Get()->Module},
                           });

            // One float4 sum per face of every target the field may write.
            m_FieldSums = Buffer::Create(
                m_Context, {
                               .Name = "Sky Bake Field Sums",
                               .Size = static_cast<u64>(MaxFieldTargets) * CubeFaces * sizeof(vec4),
                               .Usage = BufferUsage::Storage,
                               .DeviceLocal = true,
                           });
            m_FieldSumsHandle = registry.Register(m_FieldSums);
            m_FieldSamplerHandle = registry
                                       .AcquireSampler({
                                           .Name = "Sky Bake Field Sampler",
                                           .MagFilter = Filter::Linear,
                                           .MinFilter = Filter::Linear,
                                           .MipmapMode = MipmapMode::Linear,
                                           .AddressModeU = AddressMode::ClampToEdge,
                                           .AddressModeV = AddressMode::ClampToEdge,
                                           .AddressModeW = AddressMode::ClampToEdge,
                                           .MaxLod = LodClampNone,
                                       })
                                       .Handle;
        }
        if (compressed)
        {
            CreateCompressedTargets(*assets);
        }

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
        // trilinear clamp sampler at the sampler binding — the IBL consumer set's shape for those
        // two. The LOD is unclamped so the skybox minifies through the whole chain.
        const Ref<Sampler> sampler =
            Sampler::Create(m_Context, {
                                           .Name = "Sky Bake Sampler",
                                           .MagFilter = Filter::Linear,
                                           .MinFilter = Filter::Linear,
                                           .MipmapMode = MipmapMode::Linear,
                                           .AddressModeU = AddressMode::ClampToEdge,
                                           .AddressModeV = AddressMode::ClampToEdge,
                                           .AddressModeW = AddressMode::ClampToEdge,
                                           .AnisotropyEnabled = false,
                                           .MaxLod = LodClampNone,
                                       });
        m_ConsumerSet = DescriptorSet::Create(
            m_Context, {.Name = "Sky Bake Consumer Set", .Layout = consumerLayout});
        m_ConsumerSet->Write(RadianceBinding, m_CubeView);
        m_ConsumerSet->Write(SamplerBinding, sampler);

        // Clear the displayed cube (and a compressed cube's lighting reduction) to black and leave
        // it sampled, so the skybox pass samples a defined cube from the very first frame — the
        // amortized fill lands a bake into it a few frames later, and before that there is nothing
        // baked to show. A compressed image cannot be cleared, so it takes copies of a zeroed
        // buffer instead: an all-zero BC6H block decodes to black. The stand-in depth is cleared to
        // the far-plane value (reverse-Z: 0.0). All of it is setup work, so a cube built mid-frame
        // records it into the frame rather than waiting behind it.
        const Ref<Buffer> zeroBlocks =
            compressed
                ? Buffer::Create(m_Context,
                                 {
                                     .Name = "Sky Bake Zero Blocks",
                                     .Size = static_cast<u64>(BlocksAlong(m_FaceSize)) *
                                             BlocksAlong(m_FaceSize) * EncodeBlockBytes,
                                     .Usage = BufferUsage::TransferSrc | BufferUsage::TransferDst,
                                     .DeviceLocal = true,
                                 })
                : nullptr;
        m_Context.RecordSetupCommands(
            [cubeImage = m_CubeImage, cubeView = m_CubeView, lightingImage = m_LightingImage,
             lightingView = m_LightingView, depthImage = m_DepthImage, depthView = m_DepthView,
             zeroBlocks, mipCount = m_MipCount, faceSize = m_FaceSize](CommandBuffer& cmd)
            {
                const vec4 black{0.0f, 0.0f, 0.0f, 1.0f};
                const vec4 farPlane{0.0f, 0.0f, 0.0f, 0.0f};
                const auto clear =
                    [&](const Ref<Image>& image, const Ref<ImageView>& view, const vec4 value)
                {
                    cmd.PrepareForAccess(view, AccessKind::TransferDst);
                    cmd.ClearColorImage(image, value);
                    cmd.PrepareForAccess(view, AccessKind::SampleAny);
                };
                if (zeroBlocks)
                {
                    cmd.FillBuffer(zeroBlocks, 0);
                    cmd.PrepareForAccess(zeroBlocks, {AccessKind::TransferDst},
                                         {AccessKind::TransferSrc});
                    // Every subresource reads the same zeroed prefix of the buffer.
                    vector<BufferImageCopyRegion> regions;
                    for (u32 mip = 0; mip < mipCount; ++mip)
                    {
                        const u32 size = std::max(1u, faceSize >> mip);
                        for (u32 face = 0; face < CubeFaces; ++face)
                        {
                            regions.push_back({.MipLevel = mip,
                                               .BaseArrayLayer = face,
                                               .ArrayLayers = 1,
                                               .Extent = {size, size, 1}});
                        }
                    }
                    cmd.PrepareForAccess(cubeView, AccessKind::TransferDst);
                    cmd.CopyBufferToImage(zeroBlocks, cubeImage, regions);
                    cmd.PrepareForAccess(cubeView, AccessKind::SampleAny);
                    clear(lightingImage, lightingView, black);
                }
                else
                {
                    clear(cubeImage, cubeView, black);
                }
                clear(depthImage, depthView, farPlane);
            });
    }

    BakedSkyCube::~BakedSkyCube()
    {
        // Tear down any amortized bake still in flight so its tick/completion callbacks — which
        // capture this — never fire after it is gone.
        CancelBake();
        BindlessRegistry& registry = m_Context.GetBindlessRegistry();
        registry.Release(m_DepthHandle);
        for (u32 i = 0; i < m_FieldTargetsCreated; ++i)
        {
            registry.Release(m_FieldTargets[i].Handle);
        }
        if (m_FieldSumsHandle.IsValid())
        {
            registry.Release(m_FieldSumsHandle);
        }
    }

    Ref<GraphicsPipeline> BakedSkyCube::CreateBakePipeline(const MaterialInstance& material,
                                                           const BlendState& blend,
                                                           const u32 fieldTargets)
    {
        VE_ASSERT(material.GetDomain() == MaterialDomain::Sky,
                  "BakedSkyCube: material '{}' is not a Sky material", material.GetName());

        // The material's own fragment + the fullscreen vertex, against the cube-face color format with
        // the layer's blend on the one attachment — or a field's targets, one attachment each. The
        // layout (set 0 reserved, the sky push range) comes from the material loader, so the fragment
        // binds and pushes exactly as in the direct path — it is unchanged and unaware of the bake.
        vector<PipelineAttachmentInfo> attachments;
        if (fieldTargets == 0)
        {
            attachments.push_back({.Format = m_SceneColorFormat, .Blend = blend});
        }
        for (u32 i = 0; i < fieldTargets; ++i)
        {
            attachments.push_back({.Format = FieldFormat, .Blend = blend});
        }
        return GraphicsPipeline::Create(
            m_Context,
            {
                .Name = fmt::format("Sky Bake Pipeline ({})", material.GetName()),
                .ColorAttachments = std::move(attachments),
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
            const u32 fieldTargets =
                layer.Role == SkyBakeRole::Field ? std::max(layer.FieldTargets, 1u) : 0u;
            LayerPipeline& slot = m_LayerPipelines[i];
            // Reuse the slot's pipeline when the layer's fragment, blend and targets are unchanged —
            // a re-bake of the same stack recompiles no sky shader, the same reuse EnsurePipeline
            // gives the single-material path.
            if (slot.Pipeline && slot.Fragment == fragment && BlendEqual(slot.Blend, layer.Blend) &&
                slot.FieldTargets == fieldTargets)
            {
                continue;
            }
            slot.Pipeline = CreateBakePipeline(*layer.Material, layer.Blend, fieldTargets);
            slot.Fragment = fragment;
            slot.Blend = layer.Blend;
            slot.FieldTargets = fieldTargets;
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
        // promote does not fight it face-by-face.
        cmd.PrepareForAccess(m_CoarseScratchView, AccessKind::TransferSrc);
        cmd.PrepareForAccess(m_ScratchView, AccessKind::TransferDst);
        const i32 coarse = static_cast<i32>(m_CoarseFaceSize);
        const i32 full = static_cast<i32>(m_FaceSize);
        cmd.BlitImage({
            .SourceImage = m_CoarseScratchImage,
            .DestinationImage = m_ScratchImage,
            .SourceMipLevel = 0,
            .DestinationMipLevel = 0,
            .SourceOffset = {0, 0, 0},
            .DestinationOffset = {0, 0, 0},
            .SourceExtent = {coarse, coarse, 1},
            .DestinationExtent = {full, full, 1},
            .LayerCount = CubeFaces,
        });
    }

    void BakedSkyCube::EnsureFieldTargets(const u32 faceSize, const u32 count)
    {
        VE_ASSERT(m_FieldSumPipeline != nullptr,
                  "BakedSkyCube: a field bake needs the cube created with an AssetManager");
        BindlessRegistry& registry = m_Context.GetBindlessRegistry();

        if (m_FieldFaceSize != faceSize)
        {
            // A new face size invalidates every target; the old ones retire through the deferred
            // path and their slots with them.
            for (u32 i = 0; i < m_FieldTargetsCreated; ++i)
            {
                registry.Release(m_FieldTargets[i].Handle);
                m_FieldTargets[i] = {};
            }
            m_FieldTargetsCreated = 0;
            m_FieldFaceSize = faceSize;
            m_FieldMipCount = static_cast<u32>(std::bit_width(faceSize));
            m_FieldStanding = false;
            m_FieldKey.clear();
        }

        for (u32 i = m_FieldTargetsCreated; i < count; ++i)
        {
            FieldTarget& target = m_FieldTargets[i];
            target.Cube = Image::Create(
                m_Context, {
                               .Name = fmt::format("Sky Bake Field {}", i),
                               .Extent = {faceSize, faceSize, 1},
                               .MipLevels = m_FieldMipCount,
                               .Layers = CubeFaces,
                               .Format = FieldFormat,
                               .Usage = ImageUsage::Sampled | ImageUsage::ColorAttachment |
                                        ImageUsage::TransferSrc | ImageUsage::TransferDst,
                           });
            target.CubeView =
                ImageView::Create(m_Context, {
                                                 .Name = fmt::format("Sky Bake Field {} Cube", i),
                                                 .Image = target.Cube,
                                                 .ViewType = ImageViewType::Cube,
                                                 .MipLevels = m_FieldMipCount,
                                                 .ArrayLayers = CubeFaces,
                                             });
            target.SumView =
                ImageView::Create(m_Context, {
                                                 .Name = fmt::format("Sky Bake Field {} Sum", i),
                                                 .Image = target.Cube,
                                                 .ViewType = ImageViewType::Array2D,
                                                 .MipLevels = 1,
                                                 .ArrayLayers = CubeFaces,
                                             });
            for (u32 face = 0; face < CubeFaces; ++face)
            {
                target.FaceViews[face] = ImageView::Create(
                    m_Context, {
                                   .Name = fmt::format("Sky Bake Field {} Face {}", i, face),
                                   .Image = target.Cube,
                                   .ViewType = ImageViewType::Type2D,
                                   .MipLevels = 1,
                                   .BaseArrayLayer = face,
                                   .ArrayLayers = 1,
                               });
            }
            target.MipViews.clear();
            for (u32 mip = 0; mip < m_FieldMipCount; ++mip)
            {
                target.MipViews.push_back(ImageView::Create(
                    m_Context, {
                                   .Name = fmt::format("Sky Bake Field {} Mip {}", i, mip),
                                   .Image = target.Cube,
                                   .ViewType = ImageViewType::Array2D,
                                   .BaseMipLevel = mip,
                                   .MipLevels = 1,
                                   .ArrayLayers = CubeFaces,
                               }));
            }
            target.Handle = registry.RegisterCube(target.CubeView);
            target.SumSet = DescriptorSet::Create(
                m_Context, {.Name = "Sky Bake Field Sum Set", .Layout = m_FieldSumSetLayout});
            target.SumSet->Write(0, target.SumView);
            target.SumSet->Write(1, m_FieldSums);
        }
        m_FieldTargetsCreated = std::max(m_FieldTargetsCreated, count);
    }

    void BakedSkyCube::RecordFieldFinish(CommandBuffer& cmd, const u32 count)
    {
        for (u32 t = 0; t < count; ++t)
        {
            const FieldTarget& target = m_FieldTargets[t];
            // A 2x2 box per level: a linear blit at exactly half the extent averages each quad.
            u32 size = m_FieldFaceSize;
            for (u32 dst = 1; dst < m_FieldMipCount; ++dst)
            {
                cmd.PrepareForAccess(target.MipViews[dst - 1], AccessKind::TransferSrc);
                cmd.PrepareForAccess(target.MipViews[dst], AccessKind::TransferDst);
                const u32 half = std::max(1u, size / 2);
                RecordHalvingBlit(cmd, target.Cube, dst, size, 0, CubeFaces);
                size = half;
            }
            cmd.PrepareForAccess(target.SumView, AccessKind::SampleCompute);
        }

        // An earlier bake's later layers read the sums in their fragments: order this write after
        // those reads, and this bake's reads after the write.
        cmd.PrepareForAccess(m_FieldSums,
                             {AccessKind::StorageBufferReadGraphics, AccessKind::StorageBufferRead},
                             {AccessKind::StorageBufferWrite});
        cmd.BindPipeline(m_FieldSumPipeline);
        m_Context.GetBindlessRegistry().Bind(cmd, PipelineBindPoint::Compute);
        for (u32 t = 0; t < count; ++t)
        {
            cmd.BindDescriptorSets(DescriptorSetBindInfo{
                .Sets = {m_FieldTargets[t].SumSet},
                .FirstSet = BindlessRegistry::FirstUserSet,
                .PipelineBindPoint = PipelineBindPoint::Compute,
            });
            cmd.PushConstants(
                FieldSumPush{.FaceSize = m_FieldFaceSize, .OutputBase = t * CubeFaces});
            cmd.Dispatch(1, 1, CubeFaces);
            // The set binds the whole buffer, so each target's dispatch is ordered after the last
            // although it writes only its own six sums.
            if (t + 1 < count)
            {
                cmd.PrepareForAccess(m_FieldSums, {AccessKind::StorageBufferWrite},
                                     {AccessKind::StorageBufferWrite});
            }
        }
        cmd.PrepareForAccess(m_FieldSums, {AccessKind::StorageBufferWrite},
                             {AccessKind::StorageBufferReadGraphics});

        for (u32 t = 0; t < count; ++t)
        {
            cmd.PrepareForAccess(m_FieldTargets[t].CubeView, AccessKind::SampleAny);
        }
    }

    void BakedSkyCube::CreateCompressedTargets(AssetManager& assets)
    {
        const AssetResult<AssetHandle<Veng::Shader>> encodeCs =
            assets.LoadSync<Veng::Shader>(SkyBakeBc6hCompId);
        VE_ASSERT(encodeCs.has_value(), "BakedSkyCube: BC6H encoder shader load failed: {}",
                  encodeCs.error().Detail);
        m_EncodeSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "Sky Bake Encode Set Layout",
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
                               },
                       });
        const Ref<PipelineLayout> layout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "Sky Bake Encode Layout",
                .DescriptorSetLayouts = {m_EncodeSetLayout},
                .PushConstantRanges = {PushConstantRange::Of<EncodePush>(ShaderStage::Compute)},
            });
        m_EncodePipeline = ComputePipeline::Create(
            m_Context, {
                           .Name = "Sky Bake Encode Pipeline",
                           .PipelineLayout = layout,
                           .ShaderStage = {.Stage = ShaderStage::Compute,
                                           .Module = encodeCs.value().Get()->Module},
                       });

        // The lighting reduction, double-buffered: the bake writes the scratch a tile at a time and
        // RecordAmortized copies it across, so a renderer never reads one half-written.
        m_LightingFaceSize = std::min(m_FaceSize, LightingFaceSize);
        const u32 lightingMips = static_cast<u32>(std::bit_width(m_LightingFaceSize));
        const auto createLighting = [&](const char* name)
        {
            return Image::Create(m_Context,
                                 {
                                     .Name = name,
                                     .Extent = {m_LightingFaceSize, m_LightingFaceSize, 1},
                                     .MipLevels = lightingMips,
                                     .Layers = CubeFaces,
                                     .Format = m_SceneColorFormat,
                                     .Usage = ImageUsage::Sampled | ImageUsage::TransferSrc |
                                              ImageUsage::TransferDst,
                                 });
        };
        m_LightingImage = createLighting("Sky Bake Lighting Reduction");
        m_LightingView = ImageView::Create(m_Context, {
                                                          .Name = "Sky Bake Lighting View",
                                                          .Image = m_LightingImage,
                                                          .ViewType = ImageViewType::Cube,
                                                          .MipLevels = lightingMips,
                                                          .ArrayLayers = CubeFaces,
                                                      });
        m_LightingBaseMip = 0;
        m_ShReadbackMip = FirstMipAtOrBelow(m_LightingFaceSize, ShReadbackFaceSize);
        m_ShReadbackFaceSize = std::max(1u, m_LightingFaceSize >> m_ShReadbackMip);

        m_LightingScratchImage = createLighting("Sky Bake Lighting Scratch");
        m_LightingScratchView =
            ImageView::Create(m_Context, {
                                             .Name = "Sky Bake Lighting Scratch View",
                                             .Image = m_LightingScratchImage,
                                             .ViewType = ImageViewType::Array2D,
                                             .MipLevels = lightingMips,
                                             .ArrayLayers = CubeFaces,
                                         });
        for (u32 mip = 0; mip < lightingMips; ++mip)
        {
            m_LightingScratchMipViews.push_back(ImageView::Create(
                m_Context, {
                               .Name = fmt::format("Sky Bake Lighting Scratch Mip {}", mip),
                               .Image = m_LightingScratchImage,
                               .ViewType = ImageViewType::Array2D,
                               .BaseMipLevel = mip,
                               .MipLevels = 1,
                               .ArrayLayers = CubeFaces,
                           }));
        }
    }

    void BakedSkyCube::EnsureEncodeTile(const u32 tilePixels)
    {
        if (m_TileImage && m_EncodeTilePixels == tilePixels)
        {
            return;
        }

        // The buffer holds one step's blocks: a tile's encoded mips, or the six faces of the coarse
        // mips the finish encodes from the lighting reduction.
        const u32 tileMips = static_cast<u32>(std::bit_width(tilePixels));
        const u32 encodedTileMips = tileMips - 2;
        u64 tileBlocks = 0;
        for (u32 mip = 0; mip < encodedTileMips; ++mip)
        {
            const u64 blocks = BlocksAlong(tilePixels >> mip);
            tileBlocks += blocks * blocks;
        }
        u64 coarseBlocks = 0;
        for (u32 mip = encodedTileMips; mip < m_MipCount; ++mip)
        {
            const u64 blocks = BlocksAlong(std::max(1u, m_FaceSize >> mip));
            coarseBlocks += blocks * blocks * CubeFaces;
        }
        const u64 capacity = std::max(tileBlocks, coarseBlocks);
        if (capacity > m_EncodeBlockCapacity)
        {
            m_EncodeBlocks = Buffer::Create(
                m_Context, {
                               .Name = "Sky Bake Encoded Blocks",
                               .Size = capacity * EncodeBlockBytes,
                               .Usage = BufferUsage::Storage | BufferUsage::TransferSrc,
                               .DeviceLocal = true,
                           });
            m_EncodeBlockCapacity = capacity;
        }

        m_EncodeTilePixels = tilePixels;
        m_TileImage = Image::Create(m_Context,
                                    {
                                        .Name = "Sky Bake Encode Tile",
                                        .Extent = {tilePixels, tilePixels, 1},
                                        .MipLevels = tileMips,
                                        .Format = m_SceneColorFormat,
                                        .Usage = ImageUsage::Sampled | ImageUsage::ColorAttachment |
                                                 ImageUsage::TransferSrc | ImageUsage::TransferDst,
                                    });
        m_TileRenderView = ImageView::Create(m_Context, {
                                                            .Name = "Sky Bake Encode Tile Render",
                                                            .Image = m_TileImage,
                                                        });
        m_TileMipViews.clear();
        m_TileEncodeSets.clear();
        for (u32 mip = 0; mip < tileMips; ++mip)
        {
            m_TileMipViews.push_back(ImageView::Create(
                m_Context, {
                               .Name = fmt::format("Sky Bake Encode Tile Mip {}", mip),
                               .Image = m_TileImage,
                               .ViewType = ImageViewType::Array2D,
                               .BaseMipLevel = mip,
                               .MipLevels = 1,
                           }));
        }
        const auto encodeSet = [&](const Ref<ImageView>& source)
        {
            Ref<DescriptorSet> set = DescriptorSet::Create(
                m_Context, {.Name = "Sky Bake Encode Set", .Layout = m_EncodeSetLayout});
            set->Write(0, source);
            set->Write(1, m_EncodeBlocks);
            return set;
        };
        for (u32 mip = 0; mip < encodedTileMips; ++mip)
        {
            m_TileEncodeSets.push_back(encodeSet(m_TileMipViews[mip]));
        }
        // Rebuilt with the tile, since they name the buffer the tile may have just replaced.
        m_LightingEncodeSets.clear();
        for (const Ref<ImageView>& view : m_LightingScratchMipViews)
        {
            m_LightingEncodeSets.push_back(encodeSet(view));
        }
    }

    u32 BakedSkyCube::RecordEncodeDispatch(CommandBuffer& cmd, const Ref<DescriptorSet>& set,
                                           const u32 sourceSize, const u32 layers,
                                           const u32 outputBase)
    {
        const u32 blocks = BlocksAlong(sourceSize);
        cmd.BindDescriptorSets(DescriptorSetBindInfo{
            .Sets = {set},
            .FirstSet = BindlessRegistry::FirstUserSet,
            .PipelineBindPoint = PipelineBindPoint::Compute,
        });
        cmd.PushConstants(EncodePush{.SourceSize = {sourceSize, sourceSize},
                                     .BlocksPerAxis = {blocks, blocks},
                                     .OutputBase = outputBase});
        cmd.Dispatch((blocks + 7) / 8, (blocks + 7) / 8, layers);
        return blocks * blocks * layers;
    }

    void BakedSkyCube::RecordTileEncode(CommandBuffer& cmd, const u32 face, const uvec2 origin)
    {
        const u32 tilePixels = m_EncodeTilePixels;
        const u32 tileMips = static_cast<u32>(m_TileMipViews.size());

        // The tile's own chain, so each face mip's share of it is a box reduction of the tile.
        for (u32 dst = 1; dst < tileMips; ++dst)
        {
            cmd.PrepareForAccess(m_TileMipViews[dst - 1], AccessKind::TransferSrc);
            cmd.PrepareForAccess(m_TileMipViews[dst], AccessKind::TransferDst);
            RecordHalvingBlit(cmd, m_TileImage, dst, tilePixels >> (dst - 1), 0, 1);
        }

        // The lighting reduction's share: the tile mip at the reduction's scale. The service holds
        // the lighting scratch in TransferDst for the job's ticks.
        const u32 lightingMip = static_cast<u32>(std::countr_zero(m_FaceSize / m_LightingFaceSize));
        cmd.PrepareForAccess(m_TileMipViews[lightingMip], AccessKind::TransferSrc);
        const u32 lightingTile = tilePixels >> lightingMip;
        const ImageCopyRegion lightingCopy{
            .SourceMipLevel = lightingMip,
            .DestinationBaseLayer = face,
            .DestinationOffset = {origin.x >> lightingMip, origin.y >> lightingMip, 0},
            .Extent = {lightingTile, lightingTile, 1},
        };
        cmd.CopyImageRegions(m_TileImage, m_LightingScratchImage, {&lightingCopy, 1});

        // Encode every tile mip of at least one whole block. The block buffer was last read by the
        // previous step's copy.
        cmd.PrepareForAccess(m_EncodeBlocks, {AccessKind::TransferSrc},
                             {AccessKind::StorageBufferWrite});
        cmd.BindPipeline(m_EncodePipeline);
        m_Context.GetBindlessRegistry().Bind(cmd, PipelineBindPoint::Compute);
        vector<BufferImageCopyRegion> regions;
        u32 written = 0;
        for (u32 mip = 0; mip < m_TileEncodeSets.size(); ++mip)
        {
            const u32 size = tilePixels >> mip;
            cmd.PrepareForAccess(m_TileMipViews[mip], AccessKind::SampleCompute);
            // Each dispatch writes its own blocks, but the set binds the whole buffer.
            if (mip > 0)
            {
                cmd.PrepareForAccess(m_EncodeBlocks, {AccessKind::StorageBufferWrite},
                                     {AccessKind::StorageBufferWrite});
            }
            regions.push_back({
                .BufferOffset = written * EncodeBlockBytes,
                .MipLevel = mip,
                .BaseArrayLayer = face,
                .ArrayLayers = 1,
                .ImageOffset = {origin.x >> mip, origin.y >> mip, 0},
                .Extent = {size, size, 1},
            });
            written += RecordEncodeDispatch(cmd, m_TileEncodeSets[mip], size, 1, written);
        }
        cmd.PrepareForAccess(m_EncodeBlocks, {AccessKind::StorageBufferWrite},
                             {AccessKind::TransferSrc});
        cmd.CopyBufferToImage(m_EncodeBlocks, m_ScratchImage, regions);
    }

    void BakedSkyCube::RecordEncodeFinish(CommandBuffer& cmd)
    {
        // The lighting reduction's chain, from the mip 0 the tiles filled.
        const u32 lightingMips = static_cast<u32>(m_LightingScratchMipViews.size());
        for (u32 dst = 1; dst < lightingMips; ++dst)
        {
            cmd.PrepareForAccess(m_LightingScratchMipViews[dst - 1], AccessKind::TransferSrc);
            cmd.PrepareForAccess(m_LightingScratchMipViews[dst], AccessKind::TransferDst);
            RecordHalvingBlit(cmd, m_LightingScratchImage, dst, m_LightingFaceSize >> (dst - 1), 0,
                              CubeFaces);
        }

        // The face mips too coarse for a tile to hold a block of, each from the lighting mip of its
        // size: both are box reductions of the same texels.
        const u32 lightingMip = static_cast<u32>(std::countr_zero(m_FaceSize / m_LightingFaceSize));
        const u32 firstCoarse = static_cast<u32>(m_TileEncodeSets.size());
        cmd.PrepareForAccess(m_EncodeBlocks, {AccessKind::TransferSrc},
                             {AccessKind::StorageBufferWrite});
        cmd.BindPipeline(m_EncodePipeline);
        m_Context.GetBindlessRegistry().Bind(cmd, PipelineBindPoint::Compute);
        vector<BufferImageCopyRegion> regions;
        u32 written = 0;
        for (u32 mip = firstCoarse; mip < m_MipCount; ++mip)
        {
            const u32 size = std::max(1u, m_FaceSize >> mip);
            const u32 source = mip - lightingMip;
            cmd.PrepareForAccess(m_LightingScratchMipViews[source], AccessKind::SampleCompute);
            if (mip > firstCoarse)
            {
                cmd.PrepareForAccess(m_EncodeBlocks, {AccessKind::StorageBufferWrite},
                                     {AccessKind::StorageBufferWrite});
            }
            regions.push_back({
                .BufferOffset = written * EncodeBlockBytes,
                .MipLevel = mip,
                .BaseArrayLayer = 0,
                .ArrayLayers = CubeFaces,
                .Extent = {size, size, 1},
            });
            written +=
                RecordEncodeDispatch(cmd, m_LightingEncodeSets[source], size, CubeFaces, written);
        }
        cmd.PrepareForAccess(m_EncodeBlocks, {AccessKind::StorageBufferWrite},
                             {AccessKind::TransferSrc});
        cmd.PrepareForAccess(m_ScratchView, AccessKind::TransferDst);
        cmd.CopyBufferToImage(m_EncodeBlocks, m_ScratchImage, regions);
        // One state across the reduction's mips, so the job's completion transition reads it right.
        for (const Ref<ImageView>& mip : m_LightingScratchMipViews)
        {
            cmd.PrepareForAccess(mip, AccessKind::SampleAny);
        }
    }

    void BakedSkyCube::RecordDisplayMips(CommandBuffer& cmd)
    {
        for (u32 dst = 1; dst < m_MipCount; ++dst)
        {
            cmd.PrepareForAccess(m_MipViews[dst - 1], AccessKind::TransferSrc);
            cmd.PrepareForAccess(m_MipViews[dst], AccessKind::TransferDst);
            RecordHalvingBlit(cmd, m_CubeImage, dst, std::max(1u, m_FaceSize >> (dst - 1)), 0,
                              CubeFaces);
        }
        // Every mip sampled for this frame's readers: the skybox in a fragment, the IBL convolution
        // in a dispatch, and the SH readback's copy. Mip by mip, since the chain left the levels in
        // different layouts and a transition reads its range's state from its first level.
        for (const Ref<ImageView>& mip : m_MipViews)
        {
            cmd.PrepareForAccess(mip, AccessKind::SampleAny);
        }
    }

    u32 BakedSkyCube::AcquireFaceViewSlot(const u32 face, const bool faceFirst,
                                          const u32 fieldTargets)
    {
        BindlessRegistry& registry = m_Context.GetBindlessRegistry();

        // One view slot per face, reused by that face's tiles within a single recording. Every tile
        // of a face writes the identical face basis, so one slot serves them all — bounding a
        // whole-cube bake to CubeFaces view slots per frame regardless of how many tiles it renders
        // or how many run in one pump (an UnlimitedCostBudget re-bake of a large face would otherwise
        // claim one slot per tile, far past MaxViewsPerFrame). A fresh slot is claimed at the face's
        // first tile and whenever the registry's view slots have reset since — an amortized face
        // split across frames re-claims each frame, since a slot claimed in a prior frame is not
        // this frame's to reference. The epoch, not the command buffer, marks the recording: two
        // frames a slot apart share one command buffer.
        if (faceFirst || registry.GetViewEpoch() != m_LastTileViewEpoch || face != m_LastTileFace ||
            fieldTargets != m_LastTileFieldTargets)
        {
            const bool claimed = registry.TryBeginView();
            VE_ASSERT(claimed,
                      "BakedSkyCube: the frame's view budget is spent; a bake tick declares its "
                      "view slot so the generated-texture pump defers it instead");
            // Every field a bake face does not read stays zero: no scene, no clock, no exposure, and
            // a zero sky rotation, which turns nothing.
            ViewConstantsBlock region{};
            region.InvViewProj = m_FaceInvViewProj[face];
            // The face basis is already a pure direction mapping with no camera translation in it, so
            // it is its own rotation-only inverse and serves SkyViewDirection unchanged.
            region.InvViewRotProj = m_FaceInvViewProj[face];
            region.CameraPosition = vec4(0.0f);
            region.RenderScaleUV = vec4(1.0f, 1.0f, 1.0f, 1.0f);
            region.MaxValidUV = vec4(1.0f, 1.0f, 1.0f, 1.0f);
            if (fieldTargets > 0)
            {
                for (u32 t = 0; t < fieldTargets; ++t)
                {
                    region.SkyBakeFieldCubes[static_cast<i32>(t)] = m_FieldTargets[t].Handle.Index;
                }
                region.SkyBakeFieldInfo = {fieldTargets, m_FieldSumsHandle.Index,
                                           m_FieldSamplerHandle.Index, m_FieldFaceSize};
            }
            registry.WriteViewConstants(std::as_bytes(std::span(&region, 1)));
            m_LastTileViewIndex = registry.GetCurrentViewConstantsIndex();
            m_LastTileViewEpoch = registry.GetViewEpoch();
            m_LastTileFace = face;
            m_LastTileFieldTargets = fieldTargets;
        }
        return m_LastTileViewIndex;
    }

    void BakedSkyCube::RecordMaterialRegion(
        CommandBuffer& cmd, const Ref<GraphicsPipeline>& pipeline, const MaterialInstance& material,
        const std::span<const Ref<ImageView>> faceViews, const u32 faceSize, const u32 face,
        const uvec2 tileOffset, const uvec2 tileExtent, const bool clear, const bool faceFirst,
        const u32 fieldTargets, const ivec2 viewportOffset)
    {
        const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
        const u32 selector = material.GetMaterialSelector();

        // Draw the material fragment through the face's fixed basis, with the far-plane stand-in in
        // the depth slot so SkyIsBackground passes.
        const u32 viewIndex = AcquireFaceViewSlot(face, faceFirst, fieldTargets);

        // Render-area = the tile, so a Clear touches only this tile (a whole-face Clear would erase
        // tiles already baked into this face); Clear the face's first tile, Load every later one.
        vector<RenderingAttachmentInfo> attachments;
        attachments.reserve(faceViews.size());
        for (const Ref<ImageView>& faceView : faceViews)
        {
            cmd.PrepareForAccess(faceView, AccessKind::ColorAttachment);
            attachments.push_back({
                .ImageView = faceView,
                .LoadOp = clear ? LoadOp::Clear : LoadOp::Load,
                .StoreOp = StoreOp::Store,
                .ClearValue = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            });
        }
        cmd.BeginRendering({
            .Offset = {static_cast<i32>(tileOffset.x), static_cast<i32>(tileOffset.y)},
            .Extent = tileExtent,
            .ColorAttachments = std::move(attachments),
        });
        cmd.BindPipeline(pipeline);
        // Viewport spans the whole face so the SV_Position → UV → world-direction mapping is the
        // full-face one; the tile render-area + scissor clip the fullscreen triangle to this tile's
        // texels, each computing the same cube direction it would in a whole-face draw.
        cmd.SetViewport(viewportOffset, {faceSize, faceSize});
        cmd.SetScissor({static_cast<i32>(tileOffset.x), static_cast<i32>(tileOffset.y)},
                       tileExtent);
        registry.Bind(cmd);
        cmd.PushConstants(SkyMaterialPushConstants{
            .MaterialOffset = selector,
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
        VE_ASSERT(!IsCompressed(), "BakedSkyCube::Bake: a BC6H cube bakes through RequestBake");
        EnsurePipeline(material);
        // The synchronous path renders whole faces: one region draw per face covering it, Clear.
        for (u32 face = 0; face < CubeFaces; ++face)
        {
            RecordMaterialRegion(cmd, m_Pipeline, material, std::span(&m_FaceViews[face], 1),
                                 m_FaceSize, face, {0, 0}, {m_FaceSize, m_FaceSize}, true, true);
        }
        RecordDisplayMips(cmd);
    }

    void BakedSkyCube::Bake(CommandBuffer& cmd, const std::span<const SkyBakeLayer> layers)
    {
        VE_ASSERT(!IsCompressed(), "BakedSkyCube::Bake: a BC6H cube bakes through RequestBake");
        EnsureLayerPipelines(layers);
        // A reduced-resolution base is amortized-path only: the promote is an extra scratch cube and a
        // blit that the whole-face synchronous path does not carry, and the sync path is a reference
        // convenience the amortized RequestBake is the shipping route past.
        for (const SkyBakeLayer& layer : layers)
        {
            VE_ASSERT(layer.FaceSizeDivisor <= 1,
                      "BakedSkyCube::Bake: FaceSizeDivisor > 1 needs the amortized RequestBake");
            VE_ASSERT(layer.Role == SkyBakeRole::Picture,
                      "BakedSkyCube::Bake: a Field layer needs the amortized RequestBake");
        }
        // The synchronous layered path renders whole faces, layer by layer: the first layer clears
        // each face, every later layer Loads and blends over it, so the finished cube is the
        // composite. Layer-major so a face's clear precedes every blend onto it.
        for (usize layer = 0; layer < layers.size(); ++layer)
        {
            for (u32 face = 0; face < CubeFaces; ++face)
            {
                RecordMaterialRegion(cmd, m_LayerPipelines[layer].Pipeline, *layers[layer].Material,
                                     std::span(&m_FaceViews[face], 1), m_FaceSize, face, {0, 0},
                                     {m_FaceSize, m_FaceSize}, layer == 0, true);
            }
        }
        RecordDisplayMips(cmd);
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
        const u32 viewIndex = AcquireFaceViewSlot(face, clear);

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
        VE_ASSERT(!IsCompressed(),
                  "BakedSkyCube::BakeAtmosphere: a BC6H cube bakes only Sky material layers");
        // The synchronous path renders whole faces: one region draw per face covering it, Clear.
        for (u32 face = 0; face < CubeFaces; ++face)
        {
            RecordAtmosphereRegion(cmd, pipeline, atmosphereSet, atmosphere, sunDirection,
                                   intensity, m_FaceViews[face], m_FaceSize, face, {0, 0},
                                   {m_FaceSize, m_FaceSize}, true);
        }
        RecordDisplayMips(cmd);
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
                                   const std::span<const SkyBakeLayer> layers, string cacheKey,
                                   string fieldKey, const u32 encodeTilePixels)
    {
        VE_ASSERT(!layers.empty(), "BakedSkyCube::RequestBake: a bake needs at least one layer");

        // Supersede whatever bake is outstanding: a fresh dirty signal wants the newest content in
        // the scratch cube, not a completed or half-filled older one.
        CancelBake();

        // The base layer (layer 0) may bake at a reduced resolution: a Picture base into the coarse
        // scratch cube, upsampled into the full cube by a replacing blit before the finer layers
        // composite over it; a Field base into the field cubes the later layers read. Only it may,
        // because the later blended layers must Load at full resolution.
        const bool hasField = layers[0].Role == SkyBakeRole::Field;
        const u32 baseDivisor = std::max(layers[0].FaceSizeDivisor, 1u);
        VE_ASSERT(
            (baseDivisor & (baseDivisor - 1)) == 0 && m_FaceSize % baseDivisor == 0,
            "BakedSkyCube::RequestBake: base FaceSizeDivisor {} must be a power of two dividing "
            "the face size {}",
            baseDivisor, m_FaceSize);
        for (usize i = 0; i < layers.size(); ++i)
        {
            const SkyBakeLayer& layer = layers[i];
            const u32 declared = layer.Material->GetParent().Get()->GetSkyFieldTargets();
            if (layer.Role == SkyBakeRole::Field)
            {
                VE_ASSERT(i == 0,
                          "BakedSkyCube::RequestBake: only the first layer may be a Field "
                          "(layer {} is one)",
                          i);
                VE_ASSERT(layer.FieldTargets >= 1 && layer.FieldTargets <= MaxFieldTargets &&
                              layer.FieldTargets == declared,
                          "BakedSkyCube::RequestBake: the field layer names {} targets and its "
                          "material '{}' declares {}",
                          layer.FieldTargets, layer.Material->GetName(), declared);
            }
            else
            {
                VE_ASSERT(declared == 0,
                          "BakedSkyCube::RequestBake: layer {} is a Picture layer drawn with the "
                          "field material '{}'",
                          i, layer.Material->GetName());
            }
            VE_ASSERT(i == 0 || layer.FaceSizeDivisor <= 1,
                      "BakedSkyCube::RequestBake: only the base layer may set FaceSizeDivisor > 1 "
                      "(layer {} set {})",
                      i, layer.FaceSizeDivisor);
        }
        VE_ASSERT(!hasField || layers.size() > 1,
                  "BakedSkyCube::RequestBake: a field layer needs a Picture layer after it");
        const bool compressed = IsCompressed();
        VE_ASSERT(!compressed || hasField || baseDivisor == 1,
                  "BakedSkyCube::RequestBake: a BC6H cube's Picture layers render whole tiles, so "
                  "its base may not set FaceSizeDivisor > 1");
        EnsureLayerPipelines(layers);

        // A compressed cube's picture layers share one tile grid, every layer of a tile in one tick.
        u32 encodeTile = 0;
        if (compressed)
        {
            const u32 smallest = std::max(4u, m_FaceSize / MaxEncodeTilesPerAxis);
            encodeTile = encodeTilePixels != 0
                             ? encodeTilePixels
                             : std::clamp(DefaultEncodeTilePixels, smallest, m_FaceSize);
            VE_ASSERT(std::has_single_bit(encodeTile) && encodeTile >= smallest &&
                          encodeTile <= m_FaceSize,
                      "BakedSkyCube::RequestBake: encode tile {} must be a power of two from {} to "
                      "the face size {}",
                      encodeTile, smallest, m_FaceSize);
            EnsureEncodeTile(encodeTile);
        }

        const u32 fieldTargets = hasField ? layers[0].FieldTargets : 0u;
        const u32 fieldFaceSize = m_FaceSize / baseDivisor;
        if (hasField)
        {
            EnsureFieldTargets(fieldFaceSize, fieldTargets);
        }
        else if (baseDivisor > 1)
        {
            EnsureCoarseScratch(fieldFaceSize);
        }
        // A standing field this request names again is read as it stands, so only the Picture
        // layers run.
        const bool runField =
            hasField && !(m_FieldStanding && !fieldKey.empty() && fieldKey == m_FieldKey &&
                          m_FieldStandingTargets == fieldTargets);

        // The job is a list of phases run in order, each one or more ticks at its own cost: a layer's
        // tiles face-major over its own grid, or a single step between layers. Each tile phase
        // captures what its ticks need by value, so the tick lambda is decoupled from
        // m_LayerPipelines (a superseding request rebuilds that in place).
        enum class PhaseKind : u8
        {
            Tiles,        // one tile of one face of a layer per tick
            Promote,      // the coarse Picture base's upsampling blit
            FieldFinish,  // the field's mips and sums
            EncodeTiles,  // one tile of one face of every Picture layer, encoded, per tick
            EncodeFinish, // the lighting reduction's mips and the coarse face mips
        };
        enum class Destination : u8
        {
            Scratch,       // the full-resolution scratch cube
            CoarseScratch, // the coarse scratch cube a reduced Picture base renders into
            Field,         // the field cubes
        };
        struct Phase
        {
            PhaseKind Kind = PhaseKind::Tiles;
            u32 Ticks = 1;
            u32 Cost = 1;
            Ref<GraphicsPipeline> Pipeline;
            const MaterialInstance* Material = nullptr;
            Destination Target = Destination::Scratch;
            u32 FaceSize = 0;
            u32 TilePixels = 0;
            u32 TilesPerAxis = 0;
            u32 TilesPerFace = 0;
            // Clear a face's first tile (a base), every tile (the first layer after a field), or none.
            bool ClearFirstTile = false;
            bool ClearEveryTile = false;
            // The field targets this layer reads, 0 for none.
            u32 ReadsField = 0;
            // The Picture layers an EncodeTiles tick renders, in order.
            vector<std::pair<Ref<GraphicsPipeline>, const MaterialInstance*>> Draws;
        };
        const auto tilePhase = [&](const usize layer, const Destination target, const u32 faceSize)
        {
            const u32 tilePixels =
                layers[layer].TilePixels != 0 ? layers[layer].TilePixels : SkyBakeTilePixels;
            const u32 tilesPerAxis = TilesPerFaceAxis(faceSize, tilePixels);
            return Phase{
                .Kind = PhaseKind::Tiles,
                .Ticks = CubeFaces * tilesPerAxis * tilesPerAxis,
                .Cost = std::max(1u, layers[layer].Cost),
                .Pipeline = m_LayerPipelines[layer].Pipeline,
                .Material = layers[layer].Material,
                .Target = target,
                .FaceSize = faceSize,
                .TilePixels = tilePixels,
                .TilesPerAxis = tilesPerAxis,
                .TilesPerFace = tilesPerAxis * tilesPerAxis,
            };
        };

        vector<Phase> phases;
        if (hasField || compressed)
        {
            if (runField)
            {
                Phase field = tilePhase(0, Destination::Field, fieldFaceSize);
                field.ClearFirstTile = true;
                phases.push_back(std::move(field));
                phases.push_back({.Kind = PhaseKind::FieldFinish});
            }
        }
        else
        {
            Phase base =
                tilePhase(0, baseDivisor > 1 ? Destination::CoarseScratch : Destination::Scratch,
                          fieldFaceSize);
            base.ClearFirstTile = true;
            phases.push_back(std::move(base));
            if (baseDivisor > 1)
            {
                phases.push_back({.Kind = PhaseKind::Promote});
            }
        }
        if (compressed)
        {
            // Each layer's Cost prices a tile of its own TilePixels, so a shared tile charges it by
            // area.
            const u32 tilesPerAxis = m_FaceSize / encodeTile;
            Phase picture{
                .Kind = PhaseKind::EncodeTiles,
                .Ticks = CubeFaces * tilesPerAxis * tilesPerAxis,
                .Cost = 0,
                .TilePixels = encodeTile,
                .TilesPerAxis = tilesPerAxis,
                .TilesPerFace = tilesPerAxis * tilesPerAxis,
                .ReadsField = fieldTargets,
            };
            for (usize layer = hasField ? 1 : 0; layer < layers.size(); ++layer)
            {
                const u64 layerTile =
                    layers[layer].TilePixels != 0 ? layers[layer].TilePixels : SkyBakeTilePixels;
                const u64 cost = static_cast<u64>(std::max(1u, layers[layer].Cost)) * encodeTile *
                                 encodeTile / (layerTile * layerTile);
                picture.Cost += static_cast<u32>(std::max<u64>(1, cost));
                picture.Draws.emplace_back(m_LayerPipelines[layer].Pipeline,
                                           layers[layer].Material);
            }
            phases.push_back(std::move(picture));
            phases.push_back({.Kind = PhaseKind::EncodeFinish});
        }
        else
        {
            for (usize layer = 1; layer < layers.size(); ++layer)
            {
                Phase picture = tilePhase(layer, Destination::Scratch, m_FaceSize);
                picture.ClearEveryTile = hasField && layer == 1;
                picture.ReadsField = fieldTargets;
                phases.push_back(std::move(picture));
            }
        }

        u32 tickCount = 0;
        vector<GeneratedTextureCostPhase> costPhases;
        costPhases.reserve(phases.size());
        for (const Phase& phase : phases)
        {
            tickCount += phase.Ticks;
            costPhases.push_back({.Ticks = phase.Ticks, .Cost = phase.Cost});
        }

        // A compressed bake's ticks write both of its targets by copies; its lighting reduction is
        // the second target so a cache stores and restores it with the chain.
        vector<GeneratedTextureTargetInfo> targets{{
            .Adopt = m_ScratchImage,
            .ProducerAccess = compressed ? AccessKind::TransferDst : AccessKind::ColorAttachment,
            .SampledViewType = ImageViewType::Cube,
        }};
        if (compressed)
        {
            targets.push_back({
                .Adopt = m_LightingScratchImage,
                .ProducerAccess = AccessKind::TransferDst,
                .SampledViewType = ImageViewType::Cube,
            });
        }

        GeneratedTextureRequest request{
            .Key = m_JobKey,
            .Name = "Sky Bake",
            .Targets = std::move(targets),
            .CacheKey = std::move(cacheKey),
            .TickCount = tickCount,
            .CostPhases = std::move(costPhases),
            // A face tile renders through a view slot of its own (AcquireFaceViewSlot).
            .ViewSlots = 1,
            .OnTick =
                [this, phases = std::move(phases), fieldTargets, fieldKey = std::move(fieldKey)](
                    CommandBuffer& cmd, const GeneratedTextureTickContext& context)
            {
                u32 idx = context.TickIndex;
                for (const Phase& phase : phases)
                {
                    if (idx >= phase.Ticks)
                    {
                        idx -= phase.Ticks;
                        continue;
                    }
                    switch (phase.Kind)
                    {
                    case PhaseKind::Promote:
                        RecordCoarsePromote(cmd);
                        return;
                    case PhaseKind::FieldFinish:
                        RecordFieldFinish(cmd, fieldTargets);
                        m_FieldStanding = true;
                        m_FieldKey = fieldKey;
                        m_FieldStandingTargets = fieldTargets;
                        return;
                    case PhaseKind::EncodeFinish:
                        RecordEncodeFinish(cmd);
                        return;
                    case PhaseKind::EncodeTiles:
                    {
                        const u32 face = idx / phase.TilesPerFace;
                        const u32 tile = idx % phase.TilesPerFace;
                        const TileRect rect =
                            TileRectFor(tile, phase.TilesPerAxis, m_FaceSize, phase.TilePixels);
                        // The tile target is the tile, so the face's viewport begins at minus the
                        // tile's origin and every fragment keeps its face mapping.
                        const ivec2 viewportOffset{-static_cast<i32>(rect.Offset.x),
                                                   -static_cast<i32>(rect.Offset.y)};
                        for (usize draw = 0; draw < phase.Draws.size(); ++draw)
                        {
                            RecordMaterialRegion(
                                cmd, phase.Draws[draw].first, *phase.Draws[draw].second,
                                std::span(&m_TileRenderView, 1), m_FaceSize, face, {0, 0},
                                rect.Extent, draw == 0, draw == 0 && tile == 0, phase.ReadsField,
                                viewportOffset);
                        }
                        RecordTileEncode(cmd, face, rect.Offset);
                        return;
                    }
                    case PhaseKind::Tiles:
                        break;
                    }

                    const u32 face = idx / phase.TilesPerFace;
                    const u32 tile = idx % phase.TilesPerFace;
                    const TileRect rect =
                        TileRectFor(tile, phase.TilesPerAxis, phase.FaceSize, phase.TilePixels);
                    std::array<Ref<ImageView>, MaxFieldTargets> views;
                    u32 viewCount = 1;
                    switch (phase.Target)
                    {
                    case Destination::Scratch:
                        views[0] = m_ScratchFaceViews[face];
                        break;
                    case Destination::CoarseScratch:
                        views[0] = m_CoarseScratchFaceViews[face];
                        break;
                    case Destination::Field:
                        // The field's first tile overwrites what stood: from here until its
                        // sums land, no field stands.
                        if (idx == 0)
                        {
                            m_FieldStanding = false;
                            m_FieldKey.clear();
                        }
                        viewCount = fieldTargets;
                        for (u32 t = 0; t < fieldTargets; ++t)
                        {
                            views[t] = m_FieldTargets[t].FaceViews[face];
                        }
                        ++m_FieldTilesRecorded;
                        break;
                    }
                    const bool clear = phase.ClearEveryTile || (phase.ClearFirstTile && tile == 0);
                    RecordMaterialRegion(cmd, phase.Pipeline, *phase.Material,
                                         std::span(views.data(), viewCount), phase.FaceSize, face,
                                         rect.Offset, rect.Extent, clear, tile == 0,
                                         phase.ReadsField);
                    return;
                }
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
        VE_ASSERT(!IsCompressed(),
                  "BakedSkyCube::RequestBakeAtmosphere: a BC6H cube bakes only Sky material "
                  "layers");
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
            .ViewSlots = 1,
            .OnTick =
                [this, pipeline, atmosphereSet, atmosphere, sunDirection,
                 intensity](CommandBuffer& cmd, const GeneratedTextureTickContext& context)
            {
                // One tile per tick: TickIndex enumerates (face, tile) in face-major order over the
                // default tile grid (an atmosphere bake is a single layer with no caller tile size).
                const u32 face = context.TickIndex / m_TilesPerFace;
                const u32 tile = context.TickIndex % m_TilesPerFace;
                const TileRect rect =
                    TileRectFor(tile, m_TilesPerAxis, m_FaceSize, SkyBakeTilePixels);
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

        // The scratch holds a complete bake. Copy it into the displayed cube in one step — an atomic
        // swap of content, so the skybox never samples a half-filled cube — and leave everything a
        // renderer reads sampled for this frame. A compressed bake carries its whole chain and its
        // lighting reduction; a float one rebuilds its chain from the fresh mip 0.
        if (IsCompressed())
        {
            cmd.PrepareForAccess(m_ScratchView, AccessKind::TransferSrc);
            cmd.PrepareForAccess(m_CubeView, AccessKind::TransferDst);
            RecordWholeChainCopy(cmd, m_ScratchImage, m_CubeImage);
            cmd.PrepareForAccess(m_CubeView, AccessKind::SampleAny);
            cmd.PrepareForAccess(m_LightingScratchView, AccessKind::TransferSrc);
            cmd.PrepareForAccess(m_LightingView, AccessKind::TransferDst);
            RecordWholeChainCopy(cmd, m_LightingScratchImage, m_LightingImage);
            cmd.PrepareForAccess(m_LightingView, AccessKind::SampleAny);
        }
        else
        {
            cmd.PrepareForAccess(m_ScratchView, AccessKind::TransferSrc);
            cmd.PrepareForAccess(m_MipViews[0], AccessKind::TransferDst);
            const ImageCopyRegion copy{
                .LayerCount = CubeFaces,
                .Extent = {m_FaceSize, m_FaceSize, 1},
            };
            cmd.CopyImageRegions(m_ScratchImage, m_CubeImage, {&copy, 1});
            RecordDisplayMips(cmd);
        }

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
        // creates a compatible layout here. Every stage that reads the ambient sees it: the
        // lighting and translucent fragments, a sprite's vertex-stage gather, a compute pass.
        constexpr ShaderStage ConsumerStages =
            ShaderStage::Vertex | ShaderStage::Fragment | ShaderStage::Compute;
        return DescriptorSetLayout::Create(context,
                                           {
                                               .Name = "Sky Radiance Consumer Set Layout",
                                               .Bindings =
                                                   {
                                                       {.Binding = 0,
                                                        .Type = DescriptorType::SampledImage,
                                                        .Count = 1,
                                                        .Stages = ConsumerStages},
                                                       {.Binding = 1,
                                                        .Type = DescriptorType::SampledImage,
                                                        .Count = 1,
                                                        .Stages = ConsumerStages},
                                                       {.Binding = 2,
                                                        .Type = DescriptorType::SampledImage,
                                                        .Count = 1,
                                                        .Stages = ConsumerStages},
                                                       {.Binding = 3,
                                                        .Type = DescriptorType::SampledImage,
                                                        .Count = 1,
                                                        .Stages = ConsumerStages},
                                                       {.Binding = 4,
                                                        .Type = DescriptorType::Sampler,
                                                        .Count = 1,
                                                        .Stages = ConsumerStages},
                                                   },
                                           });
    }
}
