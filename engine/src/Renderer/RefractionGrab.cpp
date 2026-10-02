#include "RefractionGrab.h"

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
#include <Veng/Renderer/SceneRenderer.h>
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
        // The fullscreen vertex stage shared by the copy pipeline.
        constexpr AssetId FullscreenVertId{0xF46DD3C6F2AE0628ULL};
        // The refraction scene-color copy fragment shader.
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

        // How far short of a single texel the blur chain stops: the coarsest level holds roughly an
        // 8-pixel edge on the long axis. Going further buys nothing a material can see — at that
        // size the level is already one flat colour across the pane — and each further level is a
        // pass and a barrier.
        constexpr u32 BlurTileShift = 3;

        // The number of levels the grab carries at @p extent when the blur is on. One level is what
        // the setting being off means, and it is also the floor here: a degenerate extent must not
        // produce a chain with nothing in it.
        u32 BlurLevelsFor(const uvec2 extent)
        {
            const u32 maxDim = std::max(extent.x, extent.y);
            if (maxDim == 0)
            {
                return 1;
            }
            return std::max(1u, std::bit_width(maxDim) - BlurTileShift);
        }

        // Linear float HDR format for the scene-color intermediate; the depth intermediate is R32.
        constexpr Format HdrFormat = Format::RGBA16Sfloat;
        constexpr ImageUsage HdrUsage = ImageUsage::ColorAttachment | ImageUsage::Sampled;
    }

    Unique<RefractionGrab> RefractionGrab::Create(Context& context, AssetManager& assets)
    {
        return Unique<RefractionGrab>(new RefractionGrab(context, assets));
    }

    RefractionGrab::RefractionGrab(Context& context, AssetManager& assets) : m_Context(context)
    {
        auto LoadShader = [&](const AssetId id, const char* what) -> AssetHandle<Veng::Shader>
        {
            const AssetResult<AssetHandle<Veng::Shader>> result = assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(result.has_value(), "RefractionGrab: {} shader load failed: {}", what,
                      result.error().Detail);
            return *result;
        };

        const AssetHandle<Veng::Shader> vs = LoadShader(FullscreenVertId, "fullscreen vertex");
        const AssetHandle<Veng::Shader> sceneColorCopyFs =
            LoadShader(SceneColorCopyFragId, "scene-color copy fragment");

        m_CopyLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Scene Color Copy Layout",
                           .PushConstantRanges = {PushConstantRange::Of<SceneColorCopyPush>(
                               ShaderStage::Fragment)},
                       });
        // Two attachments (the scene-color grab + the depth copy).
        m_CopyPipeline = GraphicsPipeline::Create(
            m_Context,
            {
                .Name = "SceneRenderer Scene Color Copy Pipeline",
                .ColorAttachments = {{.Format = HdrFormat}, {.Format = Format::R32Sfloat}},
                .PipelineLayout = m_CopyLayout,
                .ShaderStages =
                    {
                        {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                        {.Stage = ShaderStage::Fragment, .Module = sceneColorCopyFs.Get()->Module},
                    },
            });

        const AssetHandle<Veng::Shader> downsampleFs =
            LoadShader(SceneColorDownsampleFragId, "scene-color downsample fragment");
        m_DownsampleLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Scene Color Downsample Layout",
                           .PushConstantRanges = {PushConstantRange::Of<SceneColorDownsamplePush>(
                               ShaderStage::Fragment)},
                       });
        m_DownsamplePipeline = GraphicsPipeline::Create(
            m_Context,
            {
                .Name = "SceneRenderer Scene Color Downsample Pipeline",
                .ColorAttachments = {{.Format = HdrFormat}},
                .PipelineLayout = m_DownsampleLayout,
                .ShaderStages =
                    {
                        {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                        {.Stage = ShaderStage::Fragment, .Module = downsampleFs.Get()->Module},
                    },
            });

        // The chain's coarse tail: the level before it sampled, then one storage binding per slot.
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
            m_Context, {.Name = "SceneRenderer Scene Color Downsample Tail Set Layout",
                        .Bindings = tailBindings});
        m_TailLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "SceneRenderer Scene Color Downsample Tail Layout",
                .DescriptorSetLayouts = {m_TailSetLayout},
                .PushConstantRanges = {PushConstantRange::Of<SceneColorDownsampleTailPush>(
                    ShaderStage::Compute)},
            });
        m_TailPipeline = ComputePipeline::Create(
            m_Context,
            {
                .Name = "SceneRenderer Scene Color Downsample Tail Pipeline",
                .PipelineLayout = m_TailLayout,
                .ShaderStage = {.Stage = ShaderStage::Compute, .Module = tailCs.Get()->Module},
            });

        // The chain's own sampler. Linear *between* levels as well as within them, so a material
        // sweeping its blur crossfades rather than stepping, and MaxLod lifted off the default of 1
        // — which would otherwise pin every blurred sample to the top two levels with no error
        // anywhere. Clamped edges: a tap at the sub-rect boundary must not wrap.
        const SharedSampler chainSampler = m_Context.GetBindlessRegistry().AcquireSampler({
            .Name = "SceneRenderer Refraction Chain Sampler",
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

    RefractionGrab::~RefractionGrab()
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_SceneHandle);
        bindless.Release(m_DepthHandle);
        for (const TextureHandle handle : m_SceneMipHandles)
        {
            bindless.Release(handle);
        }
    }

    void RefractionGrab::Recreate(const SceneRendererSettings& settings, const uvec2 extent)
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_SceneHandle);
        bindless.Release(m_DepthHandle);
        for (const TextureHandle handle : m_SceneMipHandles)
        {
            bindless.Release(handle);
        }
        m_SceneHandle = {};
        m_DepthHandle = {};
        m_SceneMipViews.clear();
        m_SceneMipHandles.clear();
        m_TailSet.reset();
        m_TailFirst = 0;

        if (!settings.Refraction)
        {
            m_SceneImage.reset();
            m_SceneView.reset();
            m_DepthImage.reset();
            m_DepthView.reset();
            return;
        }

        // The pre-translucent scene color and opaque depth the copy pass fills each frame;
        // translucent materials sample them through the view block's SceneColor handles. With the
        // blur on, the scene color carries a halving chain the copy's own level is the base of, so a
        // material can read the scene behind it at a level rather than only sharp.
        const u32 levels = settings.RefractionBlur ? BlurLevelsFor(extent) : 1u;
        // The chain's coarse tail is written by a compute dispatch, which the image must allow.
        const u32 tailFirst = MipTailFirstLevel(extent, levels, 1, TailTexelCapacity);
        m_SceneImage = Image::Create(
            m_Context, {
                           .Name = "SceneRenderer Refraction Scene",
                           .Extent = {extent.x, extent.y, 1},
                           .MipLevels = levels,
                           .Format = HdrFormat,
                           .Usage = tailFirst < levels ? HdrUsage | ImageUsage::Storage : HdrUsage,
                       });
        // The whole-chain view is what a material samples a level of; the per-level views are what
        // the graph attaches to and what each halving pass reads its parent through. One view per
        // level serves both, because a level is written as an attachment and read as a texture in
        // different passes rather than at once.
        m_SceneView =
            ImageView::Create(m_Context, {
                                             .Name = "SceneRenderer Refraction Scene View",
                                             .Image = m_SceneImage,
                                             .MipLevels = levels,
                                         });
        m_SceneHandle = bindless.Register(m_SceneView);

        m_SceneMipViews.reserve(levels);
        m_SceneMipHandles.reserve(levels);
        for (u32 level = 0; level < levels; level++)
        {
            m_SceneMipViews.push_back(ImageView::Create(
                m_Context,
                {
                    .Name = fmt::format("SceneRenderer Refraction Scene Level {} View", level),
                    .Image = m_SceneImage,
                    .BaseMipLevel = level,
                    .MipLevels = 1,
                }));
            m_SceneMipHandles.push_back(bindless.Register(m_SceneMipViews.back()));
        }

        // The tail reads the level before it through the chain sampler and writes each of its
        // levels; the slots past its length repeat its last level, bound but never written.
        if (tailFirst < levels)
        {
            m_TailFirst = tailFirst;
            m_TailSet = DescriptorSet::Create(
                m_Context, {.Name = "SceneRenderer Scene Color Downsample Tail Set",
                            .Layout = m_TailSetLayout});
            m_TailSet->Write(0, m_SceneMipViews[tailFirst - 1]);
            m_TailSet->Write(1, m_Sampler);
            for (u32 slot = 0; slot < MipTailMaxLevels; slot++)
            {
                m_TailSet->Write(TailFirstStorageBinding + slot,
                                 m_SceneMipViews[std::min(tailFirst + slot, levels - 1)]);
            }
        }

        m_DepthImage = Image::Create(m_Context, {
                                                    .Name = "SceneRenderer Refraction Depth",
                                                    .Extent = {extent.x, extent.y, 1},
                                                    .Format = Format::R32Sfloat,
                                                    .Usage = HdrUsage,
                                                });
        m_DepthView = ImageView::Create(
            m_Context, {.Name = "SceneRenderer Refraction Depth View", .Image = m_DepthImage});
        m_DepthHandle = bindless.Register(m_DepthView);
    }

    void RefractionGrab::Declare(vector<Unique<ScenePass>>& passes, const ResourceId sourceId,
                                 const TextureHandle sourceHandle, const ResourceId depthId,
                                 const TextureHandle depthHandle, const ResourceId copyId,
                                 const ResourceId depthCopyId, const SamplerHandle sampler,
                                 const uvec2 extent, const std::span<const ResourceId> mipIds) const
    {
        passes.push_back(CreateUnique<SceneColorCopyScenePass>(
            m_Context, m_CopyPipeline, sourceId, sourceHandle, depthId, depthHandle, copyId,
            depthCopyId, sampler, extent));

        // One halving per level below the base, in order — each reads the level the pass before it
        // wrote, which is what the graph's Sample declaration is there to order — down to the coarse
        // tail, which one dispatch halves.
        const auto levels = static_cast<u32>(mipIds.size());
        const u32 tailFirst = m_TailSet != nullptr && m_TailFirst < levels ? m_TailFirst : levels;
        for (u32 level = 1; level < tailFirst; level++)
        {
            passes.push_back(CreateUnique<SceneColorDownsampleScenePass>(
                m_Context, m_DownsamplePipeline, mipIds[level - 1], m_SceneMipHandles[level - 1],
                mipIds[level], m_SamplerHandle, level, extent));
        }
        if (tailFirst < levels)
        {
            passes.push_back(CreateUnique<SceneColorDownsampleTailScenePass>(
                m_TailPipeline, m_TailSet, mipIds[tailFirst - 1], mipIds.subspan(tailFirst),
                tailFirst, extent));
        }
    }
}
