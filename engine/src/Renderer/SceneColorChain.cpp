#include "SceneColorChain.h"

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Types.h>

#include "MipTail.h"
#include "Passes/SceneColorCopyScenePass.h"
#include "Passes/SceneColorDownsampleScenePass.h"
#include "Passes/SceneColorDownsampleTailScenePass.h"

#include <bit>
#include <fmt/format.h>

namespace Veng::Renderer
{
    namespace
    {
        // The fullscreen vertex stage shared by the copy and halving pipelines.
        constexpr AssetId FullscreenVertId{0xF46DD3C6F2AE0628ULL};
        // The chain's level-0 copy fragment shader.
        constexpr AssetId SceneColorCopyFragId{0xBE7002B7B8E9BE5AULL};
        // The chain's halving fragment shader.
        constexpr AssetId SceneColorDownsampleFragId{0xBB7BE54192883F09ULL};
        // The chain's coarse-tail compute shader.
        constexpr AssetId SceneColorDownsampleTailCompId{0x8CD110A72061F09BULL};

        // Texels the tail dispatch's shared memory holds: RGB as halves, eight bytes a texel, within
        // a 32 KiB workgroup budget. Matches TAIL_CAPACITY in scene_color_downsample_tail.comp.
        constexpr u32 TailTexelCapacity = 3584;

        // The tail's first storage binding; one binding per tail slot follows.
        constexpr u32 TailFirstStorageBinding = 2;

        // How far short of a single texel the blur chain stops (BlurLevelsFor).
        constexpr u32 BlurTileShift = 3;

        // Linear float HDR format for the chain.
        constexpr Format HdrFormat = Format::RGBA16Sfloat;
        constexpr ImageUsage HdrUsage = ImageUsage::ColorAttachment | ImageUsage::Sampled;
    }

    Unique<SceneColorChain> SceneColorChain::Create(Context& context, AssetManager& assets,
                                                    const string_view name)
    {
        return Unique<SceneColorChain>(new SceneColorChain(context, assets, name));
    }

    SceneColorChain::SceneColorChain(Context& context, AssetManager& assets, const string_view name)
        : m_Context(context), m_Name(name)
    {
        const auto LoadShader = [&](const AssetId id, const char* what) -> AssetHandle<Veng::Shader>
        {
            const AssetResult<AssetHandle<Veng::Shader>> result = assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(result.has_value(), "SceneColorChain: {} shader load failed: {}", what,
                      result.error().Detail);
            return *result;
        };

        const AssetHandle<Veng::Shader> vs = LoadShader(FullscreenVertId, "fullscreen vertex");
        const AssetHandle<Veng::Shader> copyFs =
            LoadShader(SceneColorCopyFragId, "scene-color copy fragment");
        m_CopyLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = fmt::format("{} Copy Layout", m_Name),
                           .PushConstantRanges = {PushConstantRange::Of<SceneColorCopyPush>(
                               ShaderStage::Fragment)},
                       });
        m_CopyPipeline = GraphicsPipeline::Create(
            m_Context, {
                           .Name = fmt::format("{} Copy Pipeline", m_Name),
                           .ColorAttachments = {{.Format = HdrFormat}},
                           .PipelineLayout = m_CopyLayout,
                           .ShaderStages =
                               {
                                   {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                                   {.Stage = ShaderStage::Fragment, .Module = copyFs.Get()->Module},
                               },
                       });

        const AssetHandle<Veng::Shader> downsampleFs =
            LoadShader(SceneColorDownsampleFragId, "scene-color downsample fragment");
        m_DownsampleLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = fmt::format("{} Downsample Layout", m_Name),
                           .PushConstantRanges = {PushConstantRange::Of<SceneColorDownsamplePush>(
                               ShaderStage::Fragment)},
                       });
        m_DownsamplePipeline = GraphicsPipeline::Create(
            m_Context,
            {
                .Name = fmt::format("{} Downsample Pipeline", m_Name),
                .ColorAttachments = {{.Format = HdrFormat}},
                .PipelineLayout = m_DownsampleLayout,
                .ShaderStages =
                    {
                        {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                        {.Stage = ShaderStage::Fragment, .Module = downsampleFs.Get()->Module},
                    },
            });

        // The coarse tail: the level before it sampled, then one storage binding per slot.
        const AssetHandle<Veng::Shader> tailCs =
            LoadShader(SceneColorDownsampleTailCompId, "scene-color downsample tail compute");
        vector<DescriptorBinding> tailBindings{
            {.Binding = 0,
             .Type = DescriptorType::SampledImage,
             .Count = 1,
             .Stages = ShaderStage::Compute},
            {.Binding = 1,
             .Type = DescriptorType::Sampler,
             .Count = 1,
             .Stages = ShaderStage::Compute},
        };
        for (u32 slot = 0; slot < MipTailMaxLevels; slot++)
        {
            tailBindings.push_back({.Binding = TailFirstStorageBinding + slot,
                                    .Type = DescriptorType::StorageImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute});
        }
        m_TailSetLayout = DescriptorSetLayout::Create(
            m_Context,
            {.Name = fmt::format("{} Tail Set Layout", m_Name), .Bindings = tailBindings});
        m_TailLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = fmt::format("{} Tail Layout", m_Name),
                .DescriptorSetLayouts = {m_TailSetLayout},
                .PushConstantRanges = {PushConstantRange::Of<SceneColorDownsampleTailPush>(
                    ShaderStage::Compute)},
            });
        m_TailPipeline = ComputePipeline::Create(
            m_Context,
            {
                .Name = fmt::format("{} Tail Pipeline", m_Name),
                .PipelineLayout = m_TailLayout,
                .ShaderStage = {.Stage = ShaderStage::Compute, .Module = tailCs.Get()->Module},
            });

        // Linear *between* levels as well as within them, so a consumer sweeping its blur crossfades
        // rather than stepping, and MaxLod lifted off the default of 1 — which would otherwise pin
        // every blurred sample to the top two levels with no error anywhere. Clamped edges: a tap at
        // the sub-rect boundary must not wrap.
        const SharedSampler chainSampler = m_Context.GetBindlessRegistry().AcquireSampler({
            .Name = "SceneRenderer Scene Color Chain Sampler",
            .MagFilter = Filter::Linear,
            .MinFilter = Filter::Linear,
            .MipmapMode = MipmapMode::Linear,
            .AddressModeU = AddressMode::ClampToEdge,
            .AddressModeV = AddressMode::ClampToEdge,
            .AddressModeW = AddressMode::ClampToEdge,
            .AnisotropyEnabled = false,
            .MaxLod = LodClampNone,
        });
        m_Sampler = chainSampler.Sampler;
        m_SamplerHandle = chainSampler.Handle;
    }

    SceneColorChain::~SceneColorChain()
    {
        Release();
    }

    u32 SceneColorChain::BlurLevelsFor(const uvec2 extent)
    {
        // One level is what an unblurred chain means, and it is also the floor here: a degenerate
        // extent must not produce a chain with nothing in it.
        const u32 maxDim = std::max(extent.x, extent.y);
        if (maxDim == 0)
        {
            return 1;
        }
        return std::max(1u, std::bit_width(maxDim) - BlurTileShift);
    }

    void SceneColorChain::Release()
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_Handle);
        for (const TextureHandle handle : m_MipHandles)
        {
            bindless.Release(handle);
        }
        m_Handle = {};
        m_MipViews.clear();
        m_MipHandles.clear();
        m_TailSet.reset();
        m_TailFirst = 0;
        m_View.reset();
        m_Image.reset();
        m_Extent = uvec2{0, 0};
    }

    void SceneColorChain::Recreate(const uvec2 extent, const u32 levels)
    {
        Release();
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();

        // The chain's coarse tail is written by a compute dispatch, which the image must allow.
        const u32 tailFirst = MipTailFirstLevel(extent, levels, 1, TailTexelCapacity);
        m_Image = Image::Create(
            m_Context, {
                           .Name = m_Name,
                           .Extent = {extent.x, extent.y, 1},
                           .MipLevels = levels,
                           .Format = HdrFormat,
                           .Usage = tailFirst < levels ? HdrUsage | ImageUsage::Storage : HdrUsage,
                       });
        m_Extent = extent;
        // The whole-chain view is what a consumer samples a level of; the per-level views are what
        // the graph attaches to and what each halving pass reads its parent through. One view per
        // level serves both, because a level is written as an attachment and read as a texture in
        // different passes rather than at once.
        m_View = ImageView::Create(m_Context, {
                                                  .Name = fmt::format("{} View", m_Name),
                                                  .Image = m_Image,
                                                  .MipLevels = levels,
                                              });
        m_Handle = bindless.Register(m_View);

        m_MipViews.reserve(levels);
        m_MipHandles.reserve(levels);
        for (u32 level = 0; level < levels; level++)
        {
            m_MipViews.push_back(ImageView::Create(
                m_Context, {
                               .Name = fmt::format("{} Level {} View", m_Name, level),
                               .Image = m_Image,
                               .BaseMipLevel = level,
                               .MipLevels = 1,
                           }));
            m_MipHandles.push_back(bindless.Register(m_MipViews.back()));
        }

        // The tail reads the level before it through the chain sampler and writes each of its
        // levels; the slots past its length repeat its last level, bound but never written.
        if (tailFirst < levels)
        {
            m_TailFirst = tailFirst;
            m_TailSet = DescriptorSet::Create(
                m_Context, {.Name = fmt::format("{} Tail Set", m_Name), .Layout = m_TailSetLayout});
            m_TailSet->Write(0, m_MipViews[tailFirst - 1]);
            m_TailSet->Write(1, m_Sampler);
            for (u32 slot = 0; slot < MipTailMaxLevels; slot++)
            {
                m_TailSet->Write(TailFirstStorageBinding + slot,
                                 m_MipViews[std::min(tailFirst + slot, levels - 1)]);
            }
        }
    }

    void SceneColorChain::Declare(vector<Unique<ScenePass>>& passes, const string_view namePrefix,
                                  const ResourceId sourceId, const TextureHandle sourceHandle,
                                  const SamplerHandle sourceSampler,
                                  const std::span<const ResourceId> mipIds,
                                  const SceneColorChainRegion region, const bool* sampled) const
    {
        VE_ASSERT(IsAllocated(), "SceneColorChain: declared with no image allocated");
        const string copyName = fmt::format("{} Copy", namePrefix);
        passes.push_back(CreateUnique<SceneColorCopyScenePass>(
            m_Context, SceneColorCopyPassInfo{
                           .Name = copyName,
                           .Pipeline = m_CopyPipeline,
                           .SourceId = sourceId,
                           .SourceHandle = sourceHandle,
                           .TargetId = mipIds[0],
                           .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
                           .Sampler = sourceSampler,
                           .Extent = m_Extent,
                           .Sampled = sampled,
                           .Region = region,
                       }));

        // One halving per level below the base, in order — each reads the level the pass before it
        // wrote, which is what the graph's Sample declaration is there to order — down to the coarse
        // tail, which one dispatch halves.
        const auto levels = static_cast<u32>(mipIds.size());
        const u32 tailFirst = m_TailSet != nullptr && m_TailFirst < levels ? m_TailFirst : levels;
        for (u32 level = 1; level < tailFirst; level++)
        {
            passes.push_back(CreateUnique<SceneColorDownsampleScenePass>(
                m_Context, SceneColorDownsamplePassInfo{
                               .Name = fmt::format("{} Downsample {}", namePrefix, level),
                               .Pipeline = m_DownsamplePipeline,
                               .SourceId = mipIds[level - 1],
                               .SourceHandle = m_MipHandles[level - 1],
                               .TargetId = mipIds[level],
                               .Sampler = m_SamplerHandle,
                               .Level = level,
                               .Extent = m_Extent,
                               .Sampled = sampled,
                               .Region = region,
                           }));
        }
        if (tailFirst < levels)
        {
            passes.push_back(CreateUnique<SceneColorDownsampleTailScenePass>(
                fmt::format("{} Downsample Tail {}-{}", namePrefix, tailFirst, levels - 1),
                m_TailPipeline, m_TailSet, mipIds[tailFirst - 1], mipIds.subspan(tailFirst),
                tailFirst, m_Extent, sampled, region));
        }
    }
}
