#include "SsrChain.h"

#include "SceneRendererIds.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <span>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/RawAsset.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/DeviceCapabilities.h>
#include <Veng/Renderer/GBuffer.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Types.h>

#include <fmt/format.h>

namespace Veng::Renderer
{
    namespace
    {
        // The chain's core-pack shaders and the blue-noise sampler tables.
        constexpr AssetId DepthDownsampleCompId{0xBA94876AE3B2A2DAULL};
        constexpr AssetId ClassifyTilesCompId{0x8C382FAB5A18F8C8ULL};
        constexpr AssetId ClassifyTilesCoverageCompId{0x861A0E1D8E762BB9ULL};
        constexpr AssetId PrepareBlueNoiseCompId{0xFEB489746A920429ULL};
        constexpr AssetId PrepareIndirectArgsCompId{0x4E213E0E322BFDDEULL};
        constexpr AssetId IntersectCompId{0xAFE1A70F03527034ULL};
        constexpr AssetId ReprojectCompId{0x887C840639DEAE97ULL};
        constexpr AssetId PrefilterCompId{0x34C2D07665F81AACULL};
        constexpr AssetId ResolveTemporalCompId{0x8BA2908B318A1267ULL};
        constexpr AssetId ApplyReflectionsFragId{0x9E38E45BB3C2850DULL};
        constexpr AssetId BlueNoiseTablesId{0xFC0C39061564BE4BULL};

        // The Sobol table's extent (256 samples of 256 dimensions) and the scrambling tile's (an
        // 8-dimensional key for each of 128×128 pixels, laid out 512 wide).
        constexpr uvec2 SobolExtent{256, 256};
        constexpr uvec2 ScramblingTileExtent{512, 256};

        // The reference's default for the one tunable the view does not carry.
        constexpr u32 DefaultMostDetailedMip = 0;

        // The depth-hierarchy push block, matching ssr_depth_downsample.comp PushConstants.
        struct DownsamplePush
        {
            u32 SourceTexture;
            u32 CopiesSource;
            uvec2 DestExtent;
            uvec2 SourceExtent;
        };

        // The classifier's push block, matching ssr_classify_tiles.comp PushConstants.
        struct ClassifyPush
        {
            u32 NormalTexture;
            u32 MaterialTexture;
        };

        // The intersection's push block, matching ssr_intersect.comp PushConstants.
        struct IntersectPush
        {
            u32 NormalTexture;
            u32 ColorTexture;
        };

        // The reprojection's and prefilter's push block, matching ssr_denoiser_callbacks
        // DenoiserPushConstants (the prefilter reads only the normal).
        struct DenoiserPush
        {
            u32 NormalTexture;
            u32 MotionVectorTexture;
        };

        // The apply composite's push block, matching ssr_apply_reflections.frag PushConstants.
        struct ApplyPush
        {
            vec4 ViewDirection;
            u32 BaseColorTexture;
            u32 NormalTexture;
            u32 MaterialTexture;
            u32 Padding;
        };

        [[nodiscard]] u32 DivideRoundingUp(const u32 value, const u32 divisor)
        {
            return (value + divisor - 1) / divisor;
        }

        // The reference's hierarchy depth: min(7, ceil(log2(max(width, height)))), at least one.
        [[nodiscard]] u32 DepthHierarchyLevelCount(const uvec2 extent)
        {
            const u32 longest = std::max(extent.x, extent.y);
            const u32 ceilLog2 = longest <= 1 ? 0u : std::bit_width(longest - 1);
            return std::clamp(ceilLog2, 1u, SsrChain::MaxDepthHierarchyLevels);
        }

        [[nodiscard]] uvec2 LevelExtent(const uvec2 extent, const u32 level)
        {
            return {std::max(extent.x >> level, 1u), std::max(extent.y >> level, 1u)};
        }

        // Records a staged copy of @p bytes into @p view's image as setup work, leaving it sampled.
        void UploadAsSetup(Context& context, const Ref<ImageView>& view, std::span<const u8> bytes)
        {
            const Ref<Image> image = view->GetImage();
            const Ref<Buffer> staging =
                Buffer::Create(context, {
                                            .Name = image->GetName() + " (Upload)",
                                            .Size = bytes.size(),
                                            .Usage = BufferUsage::TransferSrc,
                                        });
            staging->UploadSync(bytes);
            context.RecordSetupCommands(
                [view, staging](CommandBuffer& cmd)
                {
                    cmd.PrepareForAccess(view, AccessKind::TransferDst);
                    cmd.CopyBufferToImage(staging, view->GetImage());
                    cmd.PrepareForAccess(view, AccessKind::SampleAny);
                });
        }

        [[nodiscard]] Ref<ImageView> CreateTarget(Context& context, const string& name,
                                                  const uvec2 extent, const Format format,
                                                  const ImageUsage usage)
        {
            const Ref<Image> image = Image::Create(context, {
                                                                .Name = name,
                                                                .Extent = {extent.x, extent.y, 1},
                                                                .Format = format,
                                                                .Usage = usage,
                                                            });
            return ImageView::Create(context, {.Name = name + " View", .Image = image});
        }

        [[nodiscard]] Ref<ComputePipeline> CreatePipeline(Context& context, AssetManager& assets,
                                                          const AssetId shader, const char* name,
                                                          const Ref<PipelineLayout>& layout)
        {
            const AssetResult<AssetHandle<Veng::Shader>> module =
                assets.LoadSync<Veng::Shader>(shader);
            VE_ASSERT(module.has_value(), "SsrChain: {} shader load failed: {}", name,
                      module.error().Detail);
            return ComputePipeline::Create(
                context, {
                             .Name = string("SSR ") + name + " Pipeline",
                             .PipelineLayout = layout,
                             .ShaderStage = {.Stage = ShaderStage::Compute,
                                             .Module = module.value().Get()->Module},
                         });
        }

        [[nodiscard]] DescriptorBinding Binding(const u32 binding, const DescriptorType type)
        {
            return {.Binding = binding, .Type = type, .Count = 1, .Stages = ShaderStage::Compute};
        }
    }

    Unique<SsrChain> SsrChain::Create(Context& context, AssetManager& assets)
    {
        return Unique<SsrChain>(new SsrChain(context, assets));
    }

    SsrChain::SsrChain(Context& context, AssetManager& assets)
        : m_Context(context), m_Assets(assets)
    {
        // The classifier compacts its rays with a subgroup ballot and prefix count.
        VE_ASSERT(m_Context.GetDeviceCapabilities().SupportsSubgroup(
                      ShaderStage::Compute, SubgroupOperation::Basic | SubgroupOperation::Ballot),
                  "SsrChain: the device does not run basic and ballot subgroup operations in "
                  "compute shaders");
        for (const Format format : {Format::RGBA16Sfloat, Format::R16Sfloat, Format::R8Unorm,
                                    Format::RG8Unorm, Format::B10G11R11Ufloat})
        {
            VE_ASSERT(m_Context.IsFormatStorageImageSupported(format),
                      "SsrChain: format {} is not a storage-image format on this device",
                      static_cast<u32>(format));
        }

        m_DownsampleSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SSR Depth Downsample Set Layout",
                           .Bindings = {Binding(0, DescriptorType::StorageImage)},
                       });
        m_DownsampleLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "SSR Depth Downsample Layout",
                .DescriptorSetLayouts = {m_DownsampleSetLayout},
                .PushConstantRanges = {PushConstantRange::Of<DownsamplePush>(ShaderStage::Compute)},
            });
        m_DownsamplePipeline = CreatePipeline(m_Context, assets, DepthDownsampleCompId,
                                              "Depth Downsample", m_DownsampleLayout);

        m_ClassifySetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SSR Classify Tiles Set Layout",
                           .Bindings =
                               {
                                   Binding(0, DescriptorType::UniformBufferDynamic),
                                   Binding(1, DescriptorType::SampledImage),
                                   Binding(2, DescriptorType::Sampler),
                                   Binding(3, DescriptorType::SampledImage),
                                   Binding(4, DescriptorType::SampledImage),
                                   Binding(5, DescriptorType::StorageImage),
                                   Binding(6, DescriptorType::StorageBuffer),
                                   Binding(7, DescriptorType::StorageBuffer),
                                   Binding(8, DescriptorType::StorageBuffer),
                                   Binding(9, DescriptorType::StorageImage),
                               },
                       });
        m_ClassifyLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "SSR Classify Tiles Layout",
                .DescriptorSetLayouts = {m_ClassifySetLayout},
                .PushConstantRanges = {PushConstantRange::Of<ClassifyPush>(ShaderStage::Compute)},
            });
        m_ClassifyPipeline = CreatePipeline(m_Context, assets, ClassifyTilesCompId,
                                            "Classify Tiles", m_ClassifyLayout);

        m_BlueNoiseSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SSR Prepare Blue Noise Set Layout",
                           .Bindings =
                               {
                                   Binding(0, DescriptorType::UniformBufferDynamic),
                                   Binding(1, DescriptorType::SampledImage),
                                   Binding(2, DescriptorType::SampledImage),
                                   Binding(3, DescriptorType::StorageImage),
                               },
                       });
        m_BlueNoiseLayout =
            PipelineLayout::Create(m_Context, {
                                                  .Name = "SSR Prepare Blue Noise Layout",
                                                  .DescriptorSetLayouts = {m_BlueNoiseSetLayout},
                                              });
        m_BlueNoisePipeline = CreatePipeline(m_Context, assets, PrepareBlueNoiseCompId,
                                             "Prepare Blue Noise", m_BlueNoiseLayout);

        m_IndirectArgsSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SSR Prepare Indirect Args Set Layout",
                           .Bindings =
                               {
                                   Binding(0, DescriptorType::StorageBuffer),
                                   Binding(1, DescriptorType::StorageBuffer),
                               },
                       });
        m_IndirectArgsLayout =
            PipelineLayout::Create(m_Context, {
                                                  .Name = "SSR Prepare Indirect Args Layout",
                                                  .DescriptorSetLayouts = {m_IndirectArgsSetLayout},
                                              });
        m_IndirectArgsPipeline = CreatePipeline(m_Context, assets, PrepareIndirectArgsCompId,
                                                "Prepare Indirect Args", m_IndirectArgsLayout);

        m_IntersectSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SSR Intersect Set Layout",
                           .Bindings =
                               {
                                   Binding(0, DescriptorType::UniformBufferDynamic),
                                   Binding(1, DescriptorType::SampledImage),
                                   Binding(2, DescriptorType::Sampler),
                                   Binding(3, DescriptorType::SampledImage),
                                   Binding(4, DescriptorType::SampledImage),
                                   Binding(5, DescriptorType::SampledImage),
                                   Binding(6, DescriptorType::StorageBuffer),
                                   Binding(7, DescriptorType::StorageBuffer),
                                   Binding(8, DescriptorType::StorageImage),
                               },
                       });
        m_IntersectLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "SSR Intersect Layout",
                .DescriptorSetLayouts = {m_IntersectSetLayout},
                .PushConstantRanges = {PushConstantRange::Of<IntersectPush>(ShaderStage::Compute)},
            });
        m_IntersectPipeline =
            CreatePipeline(m_Context, assets, IntersectCompId, "Intersect", m_IntersectLayout);

        m_ReprojectSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SSR Denoiser Reproject Set Layout",
                           .Bindings =
                               {
                                   Binding(0, DescriptorType::UniformBufferDynamic),
                                   Binding(1, DescriptorType::Sampler),
                                   Binding(2, DescriptorType::SampledImage),
                                   Binding(3, DescriptorType::SampledImage),
                                   Binding(4, DescriptorType::SampledImage),
                                   Binding(5, DescriptorType::SampledImage),
                                   Binding(6, DescriptorType::SampledImage),
                                   Binding(7, DescriptorType::SampledImage),
                                   Binding(8, DescriptorType::SampledImage),
                                   Binding(9, DescriptorType::SampledImage),
                                   Binding(10, DescriptorType::SampledImage),
                                   Binding(11, DescriptorType::StorageImage),
                                   Binding(12, DescriptorType::StorageImage),
                                   Binding(13, DescriptorType::StorageImage),
                                   Binding(14, DescriptorType::StorageBuffer),
                                   Binding(15, DescriptorType::StorageImage),
                               },
                       });
        m_ReprojectLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "SSR Denoiser Reproject Layout",
                .DescriptorSetLayouts = {m_ReprojectSetLayout},
                .PushConstantRanges = {PushConstantRange::Of<DenoiserPush>(ShaderStage::Compute)},
            });
        m_ReprojectPipeline = CreatePipeline(m_Context, assets, ReprojectCompId,
                                             "Denoiser Reproject", m_ReprojectLayout);

        m_PrefilterSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SSR Denoiser Prefilter Set Layout",
                           .Bindings =
                               {
                                   Binding(0, DescriptorType::UniformBufferDynamic),
                                   Binding(1, DescriptorType::Sampler),
                                   Binding(2, DescriptorType::SampledImage),
                                   Binding(3, DescriptorType::SampledImage),
                                   Binding(4, DescriptorType::SampledImage),
                                   Binding(5, DescriptorType::SampledImage),
                                   Binding(6, DescriptorType::SampledImage),
                                   Binding(7, DescriptorType::StorageImage),
                                   Binding(8, DescriptorType::StorageImage),
                                   Binding(9, DescriptorType::StorageBuffer),
                               },
                       });
        m_PrefilterLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "SSR Denoiser Prefilter Layout",
                .DescriptorSetLayouts = {m_PrefilterSetLayout},
                .PushConstantRanges = {PushConstantRange::Of<DenoiserPush>(ShaderStage::Compute)},
            });
        m_PrefilterPipeline = CreatePipeline(m_Context, assets, PrefilterCompId,
                                             "Denoiser Prefilter", m_PrefilterLayout);

        m_ResolveSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SSR Denoiser Resolve Temporal Set Layout",
                           .Bindings =
                               {
                                   Binding(0, DescriptorType::UniformBufferDynamic),
                                   Binding(1, DescriptorType::Sampler),
                                   Binding(2, DescriptorType::SampledImage),
                                   Binding(3, DescriptorType::SampledImage),
                                   Binding(4, DescriptorType::SampledImage),
                                   Binding(5, DescriptorType::SampledImage),
                                   Binding(6, DescriptorType::SampledImage),
                                   Binding(7, DescriptorType::SampledImage),
                                   Binding(8, DescriptorType::StorageImage),
                                   Binding(9, DescriptorType::StorageImage),
                                   Binding(10, DescriptorType::StorageBuffer),
                               },
                       });
        m_ResolveLayout =
            PipelineLayout::Create(m_Context, {
                                                  .Name = "SSR Denoiser Resolve Temporal Layout",
                                                  .DescriptorSetLayouts = {m_ResolveSetLayout},
                                              });
        m_ResolvePipeline = CreatePipeline(m_Context, assets, ResolveTemporalCompId,
                                           "Denoiser Resolve Temporal", m_ResolveLayout);

        m_ApplySetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SSR Apply Reflections Set Layout",
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
                                    .Type = DescriptorType::Sampler,
                                    .Count = 1,
                                    .Stages = ShaderStage::Fragment},
                               },
                       });
        m_ApplyLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "SSR Apply Reflections Layout",
                .DescriptorSetLayouts = {m_ApplySetLayout},
                .PushConstantRanges = {PushConstantRange::Of<ApplyPush>(ShaderStage::Fragment)},
            });
        {
            const AssetResult<AssetHandle<Veng::Shader>> vertex =
                assets.LoadSync<Veng::Shader>(FullscreenVertId);
            VE_ASSERT(vertex.has_value(), "SsrChain: fullscreen vertex shader load failed: {}",
                      vertex.error().Detail);
            const AssetResult<AssetHandle<Veng::Shader>> fragment =
                assets.LoadSync<Veng::Shader>(ApplyReflectionsFragId);
            VE_ASSERT(fragment.has_value(), "SsrChain: Apply Reflections shader load failed: {}",
                      fragment.error().Detail);
            // The sample's blend: colour One + Dst·SrcAlpha, alpha One + One.
            m_ApplyPipeline = GraphicsPipeline::Create(
                m_Context,
                {
                    .Name = "SSR Apply Reflections Pipeline",
                    .ColorAttachments = {{.Format = ApplyFormat,
                                          .Blend =
                                              {
                                                  .Enable = true,
                                                  .SrcColorFactor = BlendFactor::One,
                                                  .DstColorFactor = BlendFactor::SrcAlpha,
                                                  .ColorOp = BlendOp::Add,
                                                  .SrcAlphaFactor = BlendFactor::One,
                                                  .DstAlphaFactor = BlendFactor::One,
                                                  .AlphaOp = BlendOp::Add,
                                              }}},
                    .PipelineLayout = m_ApplyLayout,
                    .ShaderStages =
                        {
                            {.Stage = ShaderStage::Vertex, .Module = vertex.value().Get()->Module},
                            {.Stage = ShaderStage::Fragment,
                             .Module = fragment.value().Get()->Module},
                        },
                });
        }

        // The sample's apply sampler: MinMagLinearMipPoint. It sets no address mode; the composite
        // reads the LUT at saturated coordinates, so it clamps.
        m_ApplySampler = Sampler::Create(m_Context, {
                                                        .Name = "SSR Apply Reflections Sampler",
                                                        .MagFilter = Filter::Linear,
                                                        .MinFilter = Filter::Linear,
                                                        .MipmapMode = MipmapMode::Nearest,
                                                        .AddressModeU = AddressMode::ClampToEdge,
                                                        .AddressModeV = AddressMode::ClampToEdge,
                                                        .AddressModeW = AddressMode::ClampToEdge,
                                                        .AnisotropyEnabled = false,
                                                        .MaxAnisotropy = 1.0f,
                                                    });

        // The reference denoiser's one sampler: bilinear, nearest mip, clamped on every axis.
        m_DenoiserSampler = Sampler::Create(m_Context, {
                                                           .Name = "SSR Denoiser Linear Sampler",
                                                           .MagFilter = Filter::Linear,
                                                           .MinFilter = Filter::Linear,
                                                           .MipmapMode = MipmapMode::Nearest,
                                                           .AddressModeU = AddressMode::ClampToEdge,
                                                           .AddressModeV = AddressMode::ClampToEdge,
                                                           .AddressModeW = AddressMode::ClampToEdge,
                                                           .AnisotropyEnabled = false,
                                                           .MaxAnisotropy = 1.0f,
                                                       });

        // The reference's environment sampler: trilinear, clamped in U and V, wrapped in W, with no
        // level clamp.
        m_EnvironmentSampler =
            Sampler::Create(m_Context, {
                                           .Name = "SSR Environment Sampler",
                                           .MagFilter = Filter::Linear,
                                           .MinFilter = Filter::Linear,
                                           .MipmapMode = MipmapMode::Linear,
                                           .AddressModeU = AddressMode::ClampToEdge,
                                           .AddressModeV = AddressMode::ClampToEdge,
                                           .AddressModeW = AddressMode::Repeat,
                                           .AnisotropyEnabled = false,
                                           .MaxAnisotropy = 1.0f,
                                           .MaxLod = LodClampNone,
                                       });

        // The constants ring: one region per frame in flight, each on the uniform offset alignment.
        const u64 minAlign =
            GetVkPhysicalDevice(m_Context).getProperties().limits.minUniformBufferOffsetAlignment;
        const u64 alignment = std::max<u64>(minAlign, 1);
        m_ConstantsStride =
            static_cast<u32>((sizeof(SsrConstants) + alignment - 1) / alignment * alignment);
        const u32 framesInFlight = m_Context.GetMaxFramesInFlight();
        m_Constants = Buffer::Create(
            m_Context, {
                           .Name = "SSR Constants",
                           .Size = static_cast<u64>(m_ConstantsStride) * framesInFlight,
                           .Usage = BufferUsage::Uniform,
                           .HostMapped = true,
                       });
        std::memset(m_Constants->GetMappedData(), 0,
                    static_cast<usize>(m_ConstantsStride) * framesInFlight);

        // The sampler tables: one byte per entry in the asset, widened to the reference's 32-bit
        // texels.
        const AssetResult<AssetHandle<RawAsset>> tables =
            assets.LoadSync<RawAsset>(BlueNoiseTablesId);
        VE_ASSERT(tables.has_value(), "SsrChain: failed to load the blue-noise sampler tables");
        const vector<u8>& bytes = tables.value().Get()->Bytes;
        const usize sobolCount = static_cast<usize>(SobolExtent.x) * SobolExtent.y;
        const usize scramblingCount =
            static_cast<usize>(ScramblingTileExtent.x) * ScramblingTileExtent.y;
        VE_ASSERT(bytes.size() == sobolCount + scramblingCount,
                  "SsrChain: the blue-noise sampler tables are {} bytes, expected {}", bytes.size(),
                  sobolCount + scramblingCount);
        const auto Widen = [&bytes](const usize first, const usize count)
        {
            vector<u32> words(count);
            for (usize i = 0; i < count; ++i)
            {
                words[i] = bytes[first + i];
            }
            return words;
        };
        const vector<u32> sobol = Widen(0, sobolCount);
        const vector<u32> scrambling = Widen(sobolCount, scramblingCount);
        m_Sobol = CreateTarget(m_Context, "SSR Sobol Buffer", SobolExtent, Format::R32Uint,
                               ImageUsage::Sampled | ImageUsage::TransferDst);
        UploadAsSetup(
            m_Context, m_Sobol,
            std::span(reinterpret_cast<const u8*>(sobol.data()), sobol.size() * sizeof(u32)));
        m_ScramblingTile =
            CreateTarget(m_Context, "SSR Scrambling Tile Buffer", ScramblingTileExtent,
                         Format::R32Uint, ImageUsage::Sampled | ImageUsage::TransferDst);
        UploadAsSetup(m_Context, m_ScramblingTile,
                      std::span(reinterpret_cast<const u8*>(scrambling.data()),
                                scrambling.size() * sizeof(u32)));

        m_BlueNoise = CreateTarget(
            m_Context, "SSR Blue Noise Texture", uvec2(BlueNoiseSize, BlueNoiseSize),
            Format::RG8Unorm, ImageUsage::Storage | ImageUsage::Sampled | ImageUsage::TransferSrc);
    }

    SsrChain::~SsrChain()
    {
        ReleaseExtentHandles();
        RecreateCoverage(false);
    }

    void SsrChain::ReleaseExtentHandles()
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        for (const TextureHandle handle : m_DepthHierarchyHandles)
        {
            bindless.Release(handle);
        }
        m_DepthHierarchyHandles.clear();
        for (TextureHandle& handle : m_RadianceHandles)
        {
            if (handle.IsValid())
            {
                bindless.Release(handle);
            }
            handle = TextureHandle{};
        }
    }

    void SsrChain::RecreateCoverage(const bool enabled)
    {
        if (m_CoverageHandle.IsValid())
        {
            m_Context.GetBindlessRegistry().Release(m_CoverageHandle);
            m_CoverageHandle = TextureHandle{};
        }
        m_Coverage = nullptr;
        if (!enabled || m_Extent.x == 0)
        {
            return;
        }
        if (m_CoveragePipeline == nullptr)
        {
            vector<DescriptorBinding> bindings = m_ClassifySetLayout->GetBindings();
            bindings.push_back(Binding(10, DescriptorType::StorageImage));
            m_CoverageSetLayout = DescriptorSetLayout::Create(
                m_Context, {
                               .Name = "SSR Classify Tiles Coverage Set Layout",
                               .Bindings = std::move(bindings),
                           });
            m_CoverageLayout = PipelineLayout::Create(
                m_Context, {
                               .Name = "SSR Classify Tiles Coverage Layout",
                               .DescriptorSetLayouts = {m_CoverageSetLayout},
                               .PushConstantRanges = {PushConstantRange::Of<ClassifyPush>(
                                   ShaderStage::Compute)},
                           });
            m_CoveragePipeline = CreatePipeline(m_Context, m_Assets, ClassifyTilesCoverageCompId,
                                                "Classify Tiles Coverage", m_CoverageLayout);
        }
        m_Coverage =
            CreateTarget(m_Context, "SSR Coverage", m_Extent, Format::R8Unorm,
                         ImageUsage::Storage | ImageUsage::Sampled | ImageUsage::TransferSrc);
        m_CoverageHandle = m_Context.GetBindlessRegistry().Register(m_Coverage);
    }

    void SsrChain::SetCoverageOutput(const bool enabled)
    {
        if (enabled == m_CoverageWanted)
        {
            return;
        }
        m_CoverageWanted = enabled;
        if (m_DepthHierarchy != nullptr)
        {
            RecreateCoverage(enabled);
            BuildSets();
        }
    }

    void SsrChain::Recreate(const uvec2 extent)
    {
        if (extent == m_Extent && m_DepthHierarchy != nullptr)
        {
            return;
        }
        VE_ASSERT(extent.x > 0 && extent.y > 0, "SsrChain::Recreate: empty extent {}x{}", extent.x,
                  extent.y);
        // The ray list packs a pixel's x in 15 bits and its y in 14.
        VE_ASSERT(extent.x <= (1u << 15) && extent.y <= (1u << 14),
                  "SsrChain::Recreate: {}x{} exceeds the ray list's coordinate packing", extent.x,
                  extent.y);
        m_Extent = extent;
        m_FrameIndex = 0;

        ReleaseExtentHandles();
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();

        // The depth hierarchy: render resolution at level 0, seven levels at most.
        const u32 levels = DepthHierarchyLevelCount(extent);
        m_DepthHierarchy =
            Image::Create(m_Context, {
                                         .Name = "SSR Depth Hierarchy",
                                         .Extent = {extent.x, extent.y, 1},
                                         .MipLevels = levels,
                                         .Format = Format::R32Sfloat,
                                         .Usage = ImageUsage::Storage | ImageUsage::Sampled |
                                                  ImageUsage::TransferSrc,
                                     });
        m_DepthHierarchyLevels.clear();
        for (u32 level = 0; level < levels; ++level)
        {
            const Ref<ImageView> view =
                ImageView::Create(m_Context, {
                                                 .Name = "SSR Depth Hierarchy Level View",
                                                 .Image = m_DepthHierarchy,
                                                 .BaseMipLevel = level,
                                                 .MipLevels = 1,
                                             });
            m_DepthHierarchyLevels.push_back(view);
            m_DepthHierarchyHandles.push_back(bindless.Register(view));
        }
        m_DepthHierarchyView = ImageView::Create(m_Context, {
                                                                .Name = "SSR Depth Hierarchy View",
                                                                .Image = m_DepthHierarchy,
                                                                .MipLevels = levels,
                                                            });

        const ImageUsage targetUsage = ImageUsage::Storage | ImageUsage::Sampled |
                                       ImageUsage::TransferSrc | ImageUsage::TransferDst;
        for (u32 i = 0; i < 2; ++i)
        {
            m_Radiance[i] = CreateTarget(m_Context, fmt::format("SSR Radiance {}", i), extent,
                                         Format::RGBA16Sfloat, targetUsage);
            m_Variance[i] = CreateTarget(m_Context, fmt::format("SSR Variance {}", i), extent,
                                         Format::R16Sfloat, targetUsage);
            m_RadianceHandles[i] = bindless.Register(m_Radiance[i]);
        }
        RecreateCoverage(m_CoverageWanted);
        m_ExtractedRoughness =
            CreateTarget(m_Context, "SSR Extracted Roughness", extent, Format::R8Unorm,
                         ImageUsage::Storage | ImageUsage::Sampled | ImageUsage::TransferSrc);

        const uvec2 averageExtent{DivideRoundingUp(extent.x, 8), DivideRoundingUp(extent.y, 8)};
        for (u32 i = 0; i < 2; ++i)
        {
            m_SampleCount[i] =
                CreateTarget(m_Context, fmt::format("SSR Denoiser Sample Count {}", i), extent,
                             Format::R16Sfloat, targetUsage);
            m_AverageRadiance[i] =
                CreateTarget(m_Context, fmt::format("SSR Denoiser Average Radiance {}", i),
                             averageExtent, Format::B10G11R11Ufloat, targetUsage);
        }
        m_ReprojectedRadiance = CreateTarget(m_Context, "SSR Denoiser Reprojected Radiance", extent,
                                             Format::RGBA16Sfloat, targetUsage);
        const ImageUsage historyUsage =
            ImageUsage::Sampled | ImageUsage::TransferSrc | ImageUsage::TransferDst;
        m_DepthHistory = CreateTarget(m_Context, "SSR Denoiser Depth History", extent,
                                      Format::R32Sfloat, historyUsage);
        m_NormalHistory = CreateTarget(m_Context, "SSR Denoiser Normal History", extent,
                                       GBuffer::NormalFormat, historyUsage);
        m_RoughnessHistory = CreateTarget(m_Context, "SSR Denoiser Roughness History", extent,
                                          Format::R8Unorm, historyUsage);

        // The reference clears radiance, variance, sample count, average radiance and reprojected
        // radiance on its first frame, and leaves the depth, normal and roughness histories
        // uninitialized; the first frame of these targets is the one after they are created, and
        // the histories are cleared with the rest, to the g-buffer's own "no surface" (depth 0
        // under reverse-Z, a zero normal). One zeroed staging buffer the size of the widest target
        // serves every copy.
        {
            const usize bytes = static_cast<usize>(extent.x) * extent.y * 8;
            const Ref<Buffer> zeros =
                Buffer::Create(m_Context, {
                                              .Name = "SSR History Clear",
                                              .Size = bytes,
                                              .Usage = BufferUsage::TransferSrc,
                                          });
            const vector<u8> zeroBytes(bytes, 0);
            zeros->UploadSync(zeroBytes);
            const std::array<Ref<ImageView>, 12> cleared{
                m_Radiance[0],         m_Radiance[1],    m_Variance[0],        m_Variance[1],
                m_SampleCount[0],      m_SampleCount[1], m_AverageRadiance[0], m_AverageRadiance[1],
                m_ReprojectedRadiance, m_DepthHistory,   m_NormalHistory,      m_RoughnessHistory};
            m_Context.RecordSetupCommands(
                [cleared, zeros](CommandBuffer& cmd)
                {
                    for (const Ref<ImageView>& view : cleared)
                    {
                        cmd.PrepareForAccess(view, AccessKind::TransferDst);
                        cmd.CopyBufferToImage(zeros, view->GetImage());
                        cmd.PrepareForAccess(view, AccessKind::SampleAny);
                    }
                });
        }

        const u64 pixelWords = static_cast<u64>(extent.x) * extent.y;
        m_RayList = Buffer::Create(m_Context, {
                                                  .Name = "SSR Ray List",
                                                  .Size = pixelWords * sizeof(u32),
                                                  .Usage = BufferUsage::Storage,
                                                  .DeviceLocal = true,
                                              });
        m_DenoiserTileList = Buffer::Create(m_Context, {
                                                           .Name = "SSR Denoiser Tile List",
                                                           .Size = pixelWords * sizeof(u32),
                                                           .Usage = BufferUsage::Storage,
                                                           .DeviceLocal = true,
                                                       });
        // The counters start at zero; the classifier accumulates and the argument pass rotates.
        m_RayCounter = Buffer::Create(m_Context, {
                                                     .Name = "SSR Ray Counter",
                                                     .Size = CounterWords * sizeof(u32),
                                                     .Usage = BufferUsage::Storage,
                                                 });
        const std::array<u32, CounterWords> zeroCounters{};
        m_RayCounter->UploadSync(
            std::span(reinterpret_cast<const u8*>(zeroCounters.data()), sizeof(zeroCounters)));
        m_IndirectArgs =
            Buffer::Create(m_Context, {
                                          .Name = "SSR Intersection Pass Indirect Args",
                                          .Size = IndirectArgsWords * sizeof(u32),
                                          .Usage = BufferUsage::Storage | BufferUsage::Indirect,
                                          .DeviceLocal = true,
                                      });

        BuildSets();
    }

    void SsrChain::SetEnvironment(const Ref<ImageView>& cube)
    {
        if (cube == m_Environment)
        {
            return;
        }
        m_Environment = cube;
        if (m_DepthHierarchy != nullptr)
        {
            BuildSets();
        }
    }

    void SsrChain::SetBrdfLut(const Ref<ImageView>& lut)
    {
        if (lut == m_BrdfLut)
        {
            return;
        }
        m_BrdfLut = lut;
        if (m_DepthHierarchy != nullptr)
        {
            BuildSets();
        }
    }

    void SsrChain::BuildSets()
    {
        // Fresh sets rather than rewritten ones, so no set a frame still in flight bound changes.
        m_DownsampleSets.clear();
        for (const Ref<ImageView>& level : m_DepthHierarchyLevels)
        {
            const Ref<DescriptorSet> set =
                DescriptorSet::Create(m_Context, {
                                                     .Name = "SSR Depth Downsample Set",
                                                     .Layout = m_DownsampleSetLayout,
                                                 });
            set->Write(0, level);
            m_DownsampleSets.push_back(set);
        }

        for (u32 parity = 0; parity < 2; ++parity)
        {
            // The parity of the frame index: odd frames write radiance 0 and read variance 1, as
            // the reference's host selects them.
            const u32 radiance = parity != 0 ? 0u : 1u;
            const u32 varianceHistory = parity != 0 ? 1u : 0u;
            BuildDenoiserSets(parity);
            m_ApplySets[parity] = nullptr;
            if (m_BrdfLut != nullptr)
            {
                const Ref<DescriptorSet> apply =
                    DescriptorSet::Create(m_Context, {
                                                         .Name = "SSR Apply Reflections Set",
                                                         .Layout = m_ApplySetLayout,
                                                     });
                apply->Write(0, m_Radiance[radiance]);
                apply->Write(1, m_BrdfLut);
                apply->Write(2, m_ApplySampler);
                m_ApplySets[parity] = apply;
            }
            m_ClassifySets[parity] = nullptr;
            m_IntersectSets[parity] = nullptr;
            if (m_Environment == nullptr)
            {
                continue;
            }
            const Ref<DescriptorSet> set =
                DescriptorSet::Create(m_Context, {
                                                     .Name = "SSR Classify Tiles Set",
                                                     .Layout = m_ClassifySetLayout,
                                                 });
            set->Write(0, m_Constants, 0, sizeof(SsrConstants));
            set->Write(1, m_Environment);
            set->Write(2, m_EnvironmentSampler);
            set->Write(3, m_DepthHierarchyView);
            set->Write(4, m_Variance[varianceHistory]);
            set->Write(5, m_Radiance[radiance]);
            set->Write(6, m_RayList);
            set->Write(7, m_DenoiserTileList);
            set->Write(8, m_RayCounter);
            set->Write(9, m_ExtractedRoughness);
            m_ClassifySets[parity] = set;

            m_CoverageSets[parity] = nullptr;
            if (m_Coverage != nullptr)
            {
                const Ref<DescriptorSet> coverage =
                    DescriptorSet::Create(m_Context, {
                                                         .Name = "SSR Classify Tiles Coverage Set",
                                                         .Layout = m_CoverageSetLayout,
                                                     });
                coverage->Write(0, m_Constants, 0, sizeof(SsrConstants));
                coverage->Write(1, m_Environment);
                coverage->Write(2, m_EnvironmentSampler);
                coverage->Write(3, m_DepthHierarchyView);
                coverage->Write(4, m_Variance[varianceHistory]);
                coverage->Write(5, m_Radiance[radiance]);
                coverage->Write(6, m_RayList);
                coverage->Write(7, m_DenoiserTileList);
                coverage->Write(8, m_RayCounter);
                coverage->Write(9, m_ExtractedRoughness);
                coverage->Write(10, m_Coverage);
                m_CoverageSets[parity] = coverage;
            }

            const Ref<DescriptorSet> intersect =
                DescriptorSet::Create(m_Context, {
                                                     .Name = "SSR Intersect Set",
                                                     .Layout = m_IntersectSetLayout,
                                                 });
            intersect->Write(0, m_Constants, 0, sizeof(SsrConstants));
            intersect->Write(1, m_Environment);
            intersect->Write(2, m_EnvironmentSampler);
            intersect->Write(3, m_DepthHierarchyView);
            intersect->Write(4, m_ExtractedRoughness);
            intersect->Write(5, m_BlueNoise);
            intersect->Write(6, m_RayList);
            intersect->Write(7, m_RayCounter);
            intersect->Write(8, m_Radiance[radiance]);
            m_IntersectSets[parity] = intersect;
        }

        m_BlueNoiseSet = DescriptorSet::Create(m_Context, {
                                                              .Name = "SSR Prepare Blue Noise Set",
                                                              .Layout = m_BlueNoiseSetLayout,
                                                          });
        m_BlueNoiseSet->Write(0, m_Constants, 0, sizeof(SsrConstants));
        m_BlueNoiseSet->Write(1, m_Sobol);
        m_BlueNoiseSet->Write(2, m_ScramblingTile);
        m_BlueNoiseSet->Write(3, m_BlueNoise);

        m_IndirectArgsSet =
            DescriptorSet::Create(m_Context, {
                                                 .Name = "SSR Prepare Indirect Args Set",
                                                 .Layout = m_IndirectArgsSetLayout,
                                             });
        m_IndirectArgsSet->Write(0, m_RayCounter);
        m_IndirectArgsSet->Write(1, m_IndirectArgs);
    }

    void SsrChain::BuildDenoiserSets(const u32 parity)
    {
        // The reference's hosts select by the frame's parity: odd frames write radiance, variance,
        // sample count and average 0 and read their 1, even frames the other way round.
        const u32 written = parity != 0 ? 0u : 1u;
        const u32 read = 1u - written;

        const Ref<DescriptorSet> reproject =
            DescriptorSet::Create(m_Context, {
                                                 .Name = "SSR Denoiser Reproject Set",
                                                 .Layout = m_ReprojectSetLayout,
                                             });
        reproject->Write(0, m_Constants, 0, sizeof(SsrConstants));
        reproject->Write(1, m_DenoiserSampler);
        reproject->Write(2, m_DepthHierarchyView);
        reproject->Write(3, m_Radiance[written]);
        reproject->Write(4, m_Radiance[read]);
        reproject->Write(5, m_Variance[read]);
        reproject->Write(6, m_SampleCount[read]);
        reproject->Write(7, m_ExtractedRoughness);
        reproject->Write(8, m_DepthHistory);
        reproject->Write(9, m_NormalHistory);
        reproject->Write(10, m_RoughnessHistory);
        reproject->Write(11, m_Variance[written]);
        reproject->Write(12, m_SampleCount[written]);
        reproject->Write(13, m_AverageRadiance[written]);
        reproject->Write(14, m_DenoiserTileList);
        reproject->Write(15, m_ReprojectedRadiance);
        m_ReprojectSets[parity] = reproject;

        // The prefilter reads the variance the reprojection wrote and writes its result over the
        // radiance and variance histories, as the reference's host swaps them between the passes.
        const Ref<DescriptorSet> prefilter =
            DescriptorSet::Create(m_Context, {
                                                 .Name = "SSR Denoiser Prefilter Set",
                                                 .Layout = m_PrefilterSetLayout,
                                             });
        prefilter->Write(0, m_Constants, 0, sizeof(SsrConstants));
        prefilter->Write(1, m_DenoiserSampler);
        prefilter->Write(2, m_DepthHierarchyView);
        prefilter->Write(3, m_Radiance[written]);
        prefilter->Write(4, m_Variance[written]);
        prefilter->Write(5, m_AverageRadiance[read]);
        prefilter->Write(6, m_ExtractedRoughness);
        prefilter->Write(7, m_Radiance[read]);
        prefilter->Write(8, m_Variance[read]);
        prefilter->Write(9, m_DenoiserTileList);
        m_PrefilterSets[parity] = prefilter;

        // The temporal resolve reads the prefilter's output and writes the denoised reflection and
        // its variance into this frame's radiance and variance.
        const Ref<DescriptorSet> resolve =
            DescriptorSet::Create(m_Context, {
                                                 .Name = "SSR Denoiser Resolve Temporal Set",
                                                 .Layout = m_ResolveSetLayout,
                                             });
        resolve->Write(0, m_Constants, 0, sizeof(SsrConstants));
        resolve->Write(1, m_DenoiserSampler);
        resolve->Write(2, m_Radiance[read]);
        resolve->Write(3, m_Variance[read]);
        resolve->Write(4, m_SampleCount[read]);
        resolve->Write(5, m_AverageRadiance[read]);
        resolve->Write(6, m_ExtractedRoughness);
        resolve->Write(7, m_ReprojectedRadiance);
        resolve->Write(8, m_Radiance[written]);
        resolve->Write(9, m_Variance[written]);
        resolve->Write(10, m_DenoiserTileList);
        m_ResolveSets[parity] = resolve;
    }

    u32 SsrChain::ConstantsOffset() const
    {
        return m_Context.GetCurrentFrameInFlight() * m_ConstantsStride;
    }

    void SsrChain::Import(RenderGraph& graph)
    {
        VE_ASSERT(m_DepthHierarchy != nullptr, "SsrChain::Import: Recreate has not run");
        m_DepthHierarchyId =
            graph.ImportImageMips("SSR Depth Hierarchy", GetDepthHierarchyLevels());
        m_RadianceId = graph.Import("SSR Radiance");
        m_RadianceHistoryId = graph.Import("SSR Radiance History");
        m_VarianceId = graph.Import("SSR Variance");
        m_VarianceHistoryId = graph.Import("SSR Variance History");
        m_SampleCountId = graph.Import("SSR Denoiser Sample Count");
        m_SampleCountHistoryId = graph.Import("SSR Denoiser Sample Count History");
        m_AverageRadianceId = graph.Import("SSR Denoiser Average Radiance");
        m_AverageRadianceHistoryId = graph.Import("SSR Denoiser Average Radiance History");
        m_ReprojectedRadianceId = graph.Import("SSR Denoiser Reprojected Radiance");
        m_DepthHistoryId = graph.Import("SSR Denoiser Depth History");
        m_NormalHistoryId = graph.Import("SSR Denoiser Normal History");
        m_RoughnessHistoryId = graph.Import("SSR Denoiser Roughness History");
        m_ExtractedRoughnessId = graph.Import("SSR Extracted Roughness");
        m_BlueNoiseId = graph.Import("SSR Blue Noise");
        m_RayListId = graph.ImportBuffer("SSR Ray List");
        m_DenoiserTileListId = graph.ImportBuffer("SSR Denoiser Tile List");
        m_RayCounterId = graph.ImportBuffer("SSR Ray Counter");
        m_IndirectArgsId = graph.ImportBuffer("SSR Intersection Pass Indirect Args");
        m_CoverageId = m_Coverage != nullptr ? graph.Import("SSR Coverage") : ResourceId{};
    }

    void SsrChain::AppendImportBindings(vector<RenderGraph::ImportBinding>& bindings) const
    {
        for (u32 level = 0; level < GetDepthHierarchyLevels(); ++level)
        {
            bindings.push_back(
                {.Id = m_DepthHierarchyId.Level(level), .View = m_DepthHierarchyLevels[level]});
        }
        bindings.push_back({.Id = m_RadianceId, .View = m_Radiance[GetRadianceIndex()]});
        bindings.push_back(
            {.Id = m_RadianceHistoryId, .View = m_Radiance[GetRadianceHistoryIndex()]});
        bindings.push_back({.Id = m_VarianceId, .View = m_Variance[GetVarianceIndex()]});
        bindings.push_back(
            {.Id = m_VarianceHistoryId, .View = m_Variance[GetVarianceHistoryIndex()]});
        bindings.push_back({.Id = m_SampleCountId, .View = m_SampleCount[GetSampleCountIndex()]});
        bindings.push_back(
            {.Id = m_SampleCountHistoryId, .View = m_SampleCount[GetSampleCountHistoryIndex()]});
        bindings.push_back(
            {.Id = m_AverageRadianceId, .View = m_AverageRadiance[GetAverageRadianceIndex()]});
        bindings.push_back({.Id = m_AverageRadianceHistoryId,
                            .View = m_AverageRadiance[GetAverageRadianceHistoryIndex()]});
        bindings.push_back({.Id = m_ReprojectedRadianceId, .View = m_ReprojectedRadiance});
        bindings.push_back({.Id = m_DepthHistoryId, .View = m_DepthHistory});
        bindings.push_back({.Id = m_NormalHistoryId, .View = m_NormalHistory});
        bindings.push_back({.Id = m_RoughnessHistoryId, .View = m_RoughnessHistory});
        bindings.push_back({.Id = m_ExtractedRoughnessId, .View = m_ExtractedRoughness});
        bindings.push_back({.Id = m_BlueNoiseId, .View = m_BlueNoise});
        bindings.push_back({.Id = m_RayListId, .Buffer = m_RayList});
        bindings.push_back({.Id = m_DenoiserTileListId, .Buffer = m_DenoiserTileList});
        bindings.push_back({.Id = m_RayCounterId, .Buffer = m_RayCounter});
        bindings.push_back({.Id = m_IndirectArgsId, .Buffer = m_IndirectArgs});
        if (m_Coverage != nullptr)
        {
            bindings.push_back({.Id = m_CoverageId, .View = m_Coverage});
        }
    }

    void SsrChain::PrepareFrame(const SsrFrameInputs& inputs)
    {
        VE_ASSERT(inputs.RenderSize.x > 0 && inputs.RenderSize.y > 0 &&
                      inputs.RenderSize.x <= m_Extent.x && inputs.RenderSize.y <= m_Extent.y,
                  "SsrChain::PrepareFrame: a {}x{} render size does not fit the {}x{} allocation",
                  inputs.RenderSize.x, inputs.RenderSize.y, m_Extent.x, m_Extent.y);
        const SsrConstants constants{
            .InvViewProjection = glm::inverse(inputs.Projection * inputs.View),
            .Projection = inputs.Projection,
            .InvProjection = glm::inverse(inputs.Projection),
            .ViewMatrix = inputs.View,
            .InvView = glm::inverse(inputs.View),
            .PrevViewProjection = inputs.PrevViewProjection,
            .RenderSize = inputs.RenderSize,
            .InverseRenderSize = vec2(1.0f) / vec2(inputs.RenderSize),
            .IblFactor = inputs.IblFactor,
            .TemporalStabilityFactor = inputs.TemporalStabilityFactor,
            .DepthBufferThickness = inputs.DepthBufferThickness,
            .RoughnessThreshold = inputs.RoughnessThreshold,
            .VarianceThreshold = inputs.VarianceThreshold,
            .FrameIndex = m_FrameIndex,
            .MaxTraversalIntersections = inputs.MaxTraversalIntersections,
            .MinTraversalOccupancy = inputs.MinTraversalOccupancy,
            .MostDetailedMip = DefaultMostDetailedMip,
            .SamplesPerQuad = inputs.SamplesPerQuad,
            .TemporalVarianceGuidedTracingEnabled = inputs.VarianceGuidedTracing ? 1u : 0u,
            .DenoiserRenderSize = m_Extent,
            .DenoiserInverseRenderSize = vec2(1.0f) / vec2(m_Extent),
        };
        std::memcpy(static_cast<u8*>(m_Constants->GetMappedData()) + ConstantsOffset(), &constants,
                    sizeof(constants));
        m_RenderSize = inputs.RenderSize;
        // The sample's viewDirection is its camera's direction, the view-to-world z axis: under a
        // right-handed view looking down -z, the camera's backward axis, towards the eye.
        m_ViewDirection = vec3(constants.InvView[2]);
    }

    void SsrChain::Declare(RenderGraph& graph, const SsrGraphInputs& inputs)
    {
        DeclareTrace(graph, inputs);
        DeclareDenoiser(graph, inputs);
        DeclareApply(graph, inputs);
    }

    void SsrChain::DeclareTrace(RenderGraph& graph, const SsrGraphInputs& inputs)
    {
        VE_ASSERT(m_Environment != nullptr, "SsrChain::DeclareTrace: no environment cube is bound");

        // The depth hierarchy, one level per dispatch: level 0 copies the depth, each later level
        // reduces the one before it.
        const u32 levels = GetDepthHierarchyLevels();
        for (u32 level = 0; level < levels; ++level)
        {
            RenderGraph::PassBuilder builder = graph.AddComputePass("SSR Depth Downsample");
            builder.Sample(level == 0 ? inputs.Depth : m_DepthHierarchyId.Level(level - 1));
            builder.StorageWrite(m_DepthHierarchyId.Level(level));
            const TextureHandle source =
                level == 0 ? inputs.DepthHandle : m_DepthHierarchyHandles[level - 1];
            builder.Execute(
                [this, level, source](PassContext& pass)
                {
                    // The reference reduces the render-size footprint of each level.
                    const uvec2 levelExtent = LevelExtent(m_Extent, level);
                    const uvec2 footprint{DivideRoundingUp(m_RenderSize.x, 1u << level),
                                          DivideRoundingUp(m_RenderSize.y, 1u << level)};
                    const uvec2 dest = glm::min(levelExtent, footprint);
                    const DownsamplePush push{
                        .SourceTexture = source.Index,
                        .CopiesSource = level == 0 ? 1u : 0u,
                        .DestExtent = dest,
                        .SourceExtent = level == 0 ? m_Extent : LevelExtent(m_Extent, level - 1),
                    };
                    CommandBuffer& cmd = pass.Cmd();
                    cmd.BindPipeline(m_DownsamplePipeline);
                    m_Context.GetBindlessRegistry().Bind(cmd, PipelineBindPoint::Compute);
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {m_DownsampleSets[level]},
                        .FirstSet = 3,
                        .PipelineBindPoint = PipelineBindPoint::Compute,
                    });
                    cmd.PushConstants(push);
                    cmd.Dispatch(DivideRoundingUp(dest.x, 8), DivideRoundingUp(dest.y, 8), 1);
                });
        }

        {
            RenderGraph::PassBuilder builder = graph.AddComputePass("SSR Classify Tiles");
            builder.Sample(inputs.Normal);
            builder.Sample(inputs.Material);
            for (u32 level = 0; level < levels; ++level)
            {
                builder.Sample(m_DepthHierarchyId.Level(level));
            }
            builder.Sample(m_VarianceHistoryId);
            builder.StorageWrite(m_RadianceId);
            builder.StorageWrite(m_ExtractedRoughnessId);
            builder.StorageBufferWrite(m_RayListId);
            builder.StorageBufferWrite(m_DenoiserTileListId);
            // The counters are read-modify-written by the atomics.
            builder.StorageBufferRead(m_RayCounterId);
            builder.StorageBufferWrite(m_RayCounterId);
            // The coverage entry point is the same classification writing one more target.
            const bool coverage = m_Coverage != nullptr;
            if (coverage)
            {
                builder.StorageWrite(m_CoverageId);
            }
            const ClassifyPush push{
                .NormalTexture = inputs.NormalHandle.Index,
                .MaterialTexture = inputs.MaterialHandle.Index,
            };
            builder.Execute(
                [this, push, coverage](PassContext& pass)
                {
                    CommandBuffer& cmd = pass.Cmd();
                    cmd.BindPipeline(coverage ? m_CoveragePipeline : m_ClassifyPipeline);
                    m_Context.GetBindlessRegistry().Bind(cmd, PipelineBindPoint::Compute);
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {coverage ? m_CoverageSets[m_FrameIndex & 1u]
                                          : m_ClassifySets[m_FrameIndex & 1u]},
                        .FirstSet = 3,
                        .PipelineBindPoint = PipelineBindPoint::Compute,
                        .DynamicOffsets = {ConstantsOffset()},
                    });
                    cmd.PushConstants(push);
                    cmd.Dispatch(DivideRoundingUp(m_RenderSize.x, 8),
                                 DivideRoundingUp(m_RenderSize.y, 8), 1);
                });
        }

        {
            RenderGraph::PassBuilder builder = graph.AddComputePass("SSR Prepare Blue Noise");
            builder.StorageWrite(m_BlueNoiseId);
            builder.Execute(
                [this](PassContext& pass)
                {
                    CommandBuffer& cmd = pass.Cmd();
                    cmd.BindPipeline(m_BlueNoisePipeline);
                    m_Context.GetBindlessRegistry().Bind(cmd, PipelineBindPoint::Compute);
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {m_BlueNoiseSet},
                        .FirstSet = 3,
                        .PipelineBindPoint = PipelineBindPoint::Compute,
                        .DynamicOffsets = {ConstantsOffset()},
                    });
                    cmd.Dispatch(BlueNoiseSize / 8, BlueNoiseSize / 8, 1);
                });
        }

        {
            RenderGraph::PassBuilder builder = graph.AddComputePass("SSR Prepare Indirect Args");
            builder.StorageBufferRead(m_RayCounterId);
            builder.StorageBufferWrite(m_RayCounterId);
            builder.StorageBufferWrite(m_IndirectArgsId);
            builder.Execute(
                [this](PassContext& pass)
                {
                    CommandBuffer& cmd = pass.Cmd();
                    cmd.BindPipeline(m_IndirectArgsPipeline);
                    m_Context.GetBindlessRegistry().Bind(cmd, PipelineBindPoint::Compute);
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {m_IndirectArgsSet},
                        .FirstSet = 3,
                        .PipelineBindPoint = PipelineBindPoint::Compute,
                    });
                    cmd.Dispatch(1, 1, 1);
                });
        }

        {
            RenderGraph::PassBuilder builder = graph.AddComputePass("SSR Intersect");
            builder.IndirectRead(m_IndirectArgsId);
            builder.StorageBufferRead(m_RayListId);
            builder.StorageBufferRead(m_RayCounterId);
            builder.Sample(inputs.Normal);
            builder.Sample(inputs.Color);
            for (u32 level = 0; level < levels; ++level)
            {
                builder.Sample(m_DepthHierarchyId.Level(level));
            }
            builder.Sample(m_ExtractedRoughnessId);
            builder.Sample(m_BlueNoiseId);
            // Classify wrote every pixel; this writes the traced pixels and their quad copies over it.
            builder.StorageWrite(m_RadianceId);
            const IntersectPush push{
                .NormalTexture = inputs.NormalHandle.Index,
                .ColorTexture = inputs.ColorHandle.Index,
            };
            builder.Execute(
                [this, push](PassContext& pass)
                {
                    CommandBuffer& cmd = pass.Cmd();
                    cmd.BindPipeline(m_IntersectPipeline);
                    m_Context.GetBindlessRegistry().Bind(cmd, PipelineBindPoint::Compute);
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {m_IntersectSets[m_FrameIndex & 1u]},
                        .FirstSet = 3,
                        .PipelineBindPoint = PipelineBindPoint::Compute,
                        .DynamicOffsets = {ConstantsOffset()},
                    });
                    cmd.PushConstants(push);
                    // The argument pass wrote [(rays + 63) / 64, 1, 1] at byte 0.
                    cmd.DispatchIndirect(m_IndirectArgs, 0);
                });
        }
    }

    void SsrChain::DeclareDenoiser(RenderGraph& graph, const SsrGraphInputs& inputs)
    {
        const u32 levels = GetDepthHierarchyLevels();

        {
            RenderGraph::PassBuilder builder = graph.AddComputePass("SSR Denoiser Reproject");
            builder.IndirectRead(m_IndirectArgsId);
            builder.StorageBufferRead(m_DenoiserTileListId);
            builder.Sample(inputs.Normal);
            builder.Sample(inputs.Velocity);
            for (u32 level = 0; level < levels; ++level)
            {
                builder.Sample(m_DepthHierarchyId.Level(level));
            }
            builder.Sample(m_RadianceId);
            builder.Sample(m_RadianceHistoryId);
            builder.Sample(m_VarianceHistoryId);
            builder.Sample(m_SampleCountHistoryId);
            builder.Sample(m_ExtractedRoughnessId);
            builder.Sample(m_DepthHistoryId);
            builder.Sample(m_NormalHistoryId);
            builder.Sample(m_RoughnessHistoryId);
            builder.StorageWrite(m_VarianceId);
            builder.StorageWrite(m_SampleCountId);
            builder.StorageWrite(m_AverageRadianceId);
            builder.StorageWrite(m_ReprojectedRadianceId);
            const DenoiserPush push{
                .NormalTexture = inputs.NormalHandle.Index,
                .MotionVectorTexture = inputs.VelocityHandle.Index,
            };
            builder.Execute(
                [this, push](PassContext& pass)
                {
                    CommandBuffer& cmd = pass.Cmd();
                    cmd.BindPipeline(m_ReprojectPipeline);
                    m_Context.GetBindlessRegistry().Bind(cmd, PipelineBindPoint::Compute);
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {m_ReprojectSets[m_FrameIndex & 1u]},
                        .FirstSet = 3,
                        .PipelineBindPoint = PipelineBindPoint::Compute,
                        .DynamicOffsets = {ConstantsOffset()},
                    });
                    cmd.PushConstants(push);
                    cmd.DispatchIndirect(m_IndirectArgs, DenoiserArgsOffset);
                });
        }

        {
            RenderGraph::PassBuilder builder = graph.AddComputePass("SSR Denoiser Prefilter");
            builder.IndirectRead(m_IndirectArgsId);
            builder.StorageBufferRead(m_DenoiserTileListId);
            builder.Sample(inputs.Normal);
            for (u32 level = 0; level < levels; ++level)
            {
                builder.Sample(m_DepthHierarchyId.Level(level));
            }
            builder.Sample(m_RadianceId);
            builder.Sample(m_VarianceId);
            builder.Sample(m_AverageRadianceHistoryId);
            builder.Sample(m_ExtractedRoughnessId);
            builder.StorageWrite(m_RadianceHistoryId);
            builder.StorageWrite(m_VarianceHistoryId);
            const DenoiserPush push{
                .NormalTexture = inputs.NormalHandle.Index,
                .MotionVectorTexture = inputs.VelocityHandle.Index,
            };
            builder.Execute(
                [this, push](PassContext& pass)
                {
                    CommandBuffer& cmd = pass.Cmd();
                    cmd.BindPipeline(m_PrefilterPipeline);
                    m_Context.GetBindlessRegistry().Bind(cmd, PipelineBindPoint::Compute);
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {m_PrefilterSets[m_FrameIndex & 1u]},
                        .FirstSet = 3,
                        .PipelineBindPoint = PipelineBindPoint::Compute,
                        .DynamicOffsets = {ConstantsOffset()},
                    });
                    cmd.PushConstants(push);
                    cmd.DispatchIndirect(m_IndirectArgs, DenoiserArgsOffset);
                });
        }

        {
            RenderGraph::PassBuilder builder =
                graph.AddComputePass("SSR Denoiser Resolve Temporal");
            builder.IndirectRead(m_IndirectArgsId);
            builder.StorageBufferRead(m_DenoiserTileListId);
            builder.Sample(m_RadianceHistoryId);
            builder.Sample(m_VarianceHistoryId);
            builder.Sample(m_SampleCountHistoryId);
            builder.Sample(m_AverageRadianceHistoryId);
            builder.Sample(m_ExtractedRoughnessId);
            builder.Sample(m_ReprojectedRadianceId);
            builder.StorageWrite(m_RadianceId);
            builder.StorageWrite(m_VarianceId);
            builder.Execute(
                [this](PassContext& pass)
                {
                    CommandBuffer& cmd = pass.Cmd();
                    cmd.BindPipeline(m_ResolvePipeline);
                    m_Context.GetBindlessRegistry().Bind(cmd, PipelineBindPoint::Compute);
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {m_ResolveSets[m_FrameIndex & 1u]},
                        .FirstSet = 3,
                        .PipelineBindPoint = PipelineBindPoint::Compute,
                        .DynamicOffsets = {ConstantsOffset()},
                    });
                    cmd.DispatchIndirect(m_IndirectArgs, DenoiserArgsOffset);
                });
        }

        // The reference's host copies this frame's normal, extracted roughness and depth (the
        // hierarchy's level 0) into the histories next frame's reprojection reads.
        {
            RenderGraph::PassBuilder builder = graph.AddTransferPass("SSR Denoiser History Copy");
            builder.TransferSrc(inputs.Normal);
            builder.TransferSrc(m_ExtractedRoughnessId);
            builder.TransferSrc(m_DepthHierarchyId.Level(0));
            builder.TransferDst(m_NormalHistoryId);
            builder.TransferDst(m_RoughnessHistoryId);
            builder.TransferDst(m_DepthHistoryId);
            const ResourceId normal = inputs.Normal;
            builder.Execute(
                [this, normal](PassContext& pass)
                {
                    CommandBuffer& cmd = pass.Cmd();
                    cmd.CopyImage(pass.Resolved(normal).GetImage(), m_NormalHistory->GetImage());
                    cmd.CopyImage(m_ExtractedRoughness->GetImage(), m_RoughnessHistory->GetImage());
                    cmd.CopyImage(m_DepthHierarchy, m_DepthHistory->GetImage());
                });
        }
    }

    void SsrChain::DeclareApply(RenderGraph& graph, const SsrGraphInputs& inputs)
    {
        VE_ASSERT(m_BrdfLut != nullptr, "SsrChain::DeclareApply: no BRDF LUT is bound");

        RenderGraph::PassBuilder builder = graph.AddPass("SSR Apply Reflections");
        builder
            .Color({
                .Resource = inputs.Color,
                .Load = LoadOp::Load,
                .Store = StoreOp::Store,
            })
            .Sample(m_RadianceId)
            .Sample(inputs.Albedo)
            .Sample(inputs.Normal)
            .Sample(inputs.Material);
        const TextureHandle albedo = inputs.AlbedoHandle;
        const TextureHandle normal = inputs.NormalHandle;
        const TextureHandle material = inputs.MaterialHandle;
        builder.Execute(
            [this, albedo, normal, material](PassContext& pass)
            {
                CommandBuffer& cmd = pass.Cmd();
                cmd.BindPipeline(m_ApplyPipeline);
                cmd.SetViewport({0, 0}, m_RenderSize);
                cmd.SetScissor({0, 0}, m_RenderSize);
                m_Context.GetBindlessRegistry().Bind(cmd);
                cmd.BindDescriptorSets(DescriptorSetBindInfo{
                    .Sets = {m_ApplySets[m_FrameIndex & 1u]},
                    .FirstSet = 3,
                    .PipelineBindPoint = PipelineBindPoint::Graphics,
                });
                cmd.PushConstants(ApplyPush{
                    .ViewDirection = vec4(m_ViewDirection, 0.0f),
                    .BaseColorTexture = albedo.Index,
                    .NormalTexture = normal.Index,
                    .MaterialTexture = material.Index,
                    .Padding = 0,
                });
                cmd.DrawFullscreenTriangle();
            });
    }
}
