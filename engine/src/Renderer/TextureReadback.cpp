#include <Veng/Renderer/TextureReadback.h>

#include <cstring>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/FormatInfo.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>

namespace Veng::Renderer
{
    namespace
    {
        // The texel readback compute shader (one mip of an image → float RGBA storage buffer).
        constexpr AssetId TextureReadbackCompId{0x4FA1C8297F7F189DULL};

        // The readback push block, matching texture_readback.comp PushConstants.
        struct TextureReadbackPush
        {
            uvec2 Extent;
            u32 Layers;
            u32 Pad;
        };

        constexpr u32 TextureReadbackGroupSize = 8;
    }

    bool IsTexelReadable(const Format format)
    {
        switch (format)
        {
        case Format::Undefined:
        case Format::D16Unorm:
        case Format::D32Sfloat:
        case Format::S8Uint:
        case Format::D16UnormS8Uint:
        case Format::D24UnormS8Uint:
        case Format::D32SfloatS8Uint:
        case Format::X8D24UnormPack32:
        case Format::RGBA16Uint:
        case Format::R32Uint:
            return false;
        default:
            return true;
        }
    }

    bool IsSrgbFormat(const Format format)
    {
        switch (format)
        {
        case Format::RGBA8Srgb:
        case Format::BGRA8Srgb:
        case Format::BC7Srgb:
        case Format::ASTC4x4Srgb:
            return true;
        default:
            return false;
        }
    }

    Result<TexelReadback> ReadbackTexels(Context& context, AssetManager& assets,
                                         const Ref<Image>& image, const u32 mipLevel)
    {
        if (!image)
        {
            return std::unexpected(string("no image"));
        }
        if (image->GetType() != ImageType::Type2D)
        {
            return std::unexpected(string("only 2D, array and cube images read back"));
        }
        if (!HasFlag(image->GetUsage(), ImageUsage::Sampled))
        {
            return std::unexpected(fmt::format("image '{}' is not sampleable", image->GetName()));
        }
        if (!IsTexelReadable(image->GetFormat()))
        {
            return std::unexpected(fmt::format("format {} does not read back as colour",
                                               FormatName(image->GetFormat())));
        }
        if (mipLevel >= image->GetMipLevels())
        {
            return std::unexpected(fmt::format("mip {} is past the image's {} levels", mipLevel,
                                               image->GetMipLevels()));
        }

        const AssetResult<AssetHandle<Veng::Shader>> shader =
            assets.LoadSync<Veng::Shader>(TextureReadbackCompId);
        if (!shader.has_value())
        {
            return std::unexpected(
                fmt::format("readback shader failed to load: {}", shader.error().Detail));
        }

        const uvec3 baseExtent = image->GetExtent();
        const uvec2 extent{std::max(baseExtent.x >> mipLevel, 1u),
                           std::max(baseExtent.y >> mipLevel, 1u)};
        const u32 layers = image->GetLayers();
        const u64 texelCount = static_cast<u64>(extent.x) * extent.y * layers;

        const Ref<ImageView> view =
            ImageView::Create(context, {
                                           .Name = image->GetName() + " (Readback)",
                                           .Image = image,
                                           .ViewType = ImageViewType::Array2D,
                                           .BaseMipLevel = mipLevel,
                                           .MipLevels = 1,
                                           .ArrayLayers = layers,
                                       });
        const Ref<Buffer> texels =
            Buffer::Create(context, {
                                        .Name = "Texel Readback",
                                        .Size = texelCount * sizeof(vec4),
                                        .Usage = BufferUsage::Storage | BufferUsage::TransferSrc,
                                    });

        const Ref<DescriptorSetLayout> setLayout =
            DescriptorSetLayout::Create(context, {
                                                     .Name = "Texel Readback Set Layout",
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
            context, {
                         .Name = "Texel Readback Layout",
                         .DescriptorSetLayouts = {setLayout},
                         .PushConstantRanges = {PushConstantRange::Of<TextureReadbackPush>(
                             ShaderStage::Compute)},
                     });
        const Ref<ComputePipeline> pipeline = ComputePipeline::Create(
            context, {
                         .Name = "Texel Readback Pipeline",
                         .PipelineLayout = layout,
                         .ShaderStage = {.Stage = ShaderStage::Compute,
                                         .Module = shader.value().Get()->Module},
                     });
        const Ref<DescriptorSet> set =
            DescriptorSet::Create(context, {
                                               .Name = "Texel Readback Set",
                                               .Layout = setLayout,
                                           });
        set->Write(0, view);
        set->Write(1, texels);

        context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                cmd.PrepareForAccess(view, AccessKind::SampleCompute);
                cmd.BindPipeline(pipeline);
                cmd.BindDescriptorSets(DescriptorSetBindInfo{
                    .Sets = {set},
                    .FirstSet = 3,
                    .PipelineBindPoint = PipelineBindPoint::Compute,
                });
                cmd.PushConstants(TextureReadbackPush{.Extent = extent, .Layers = layers});
                cmd.Dispatch((extent.x + TextureReadbackGroupSize - 1) / TextureReadbackGroupSize,
                             (extent.y + TextureReadbackGroupSize - 1) / TextureReadbackGroupSize,
                             layers);
            });

        // ImmediateCommands has waited the dispatch's fence, so the copy Download submits next reads
        // finished texels.
        const vector<u8> bytes = texels->Download();
        TexelReadback result{
            .Extent = extent,
            .Layers = layers,
            .SourceFormat = image->GetFormat(),
        };
        result.Texels.resize(texelCount);
        std::memcpy(result.Texels.data(), bytes.data(),
                    std::min(bytes.size(), result.Texels.size() * sizeof(vec4)));
        return result;
    }
}
