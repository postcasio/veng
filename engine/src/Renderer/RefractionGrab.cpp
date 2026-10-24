#include "RefractionGrab.h"

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Types.h>

#include "Passes/SceneColorCopyScenePass.h"

namespace Veng::Renderer
{
    namespace
    {
        // The fullscreen vertex stage shared by the copy pipeline.
        constexpr AssetId FullscreenVertId{0xF46DD3C6F2AE0628ULL};
        // The grab's opaque-depth copy fragment shader.
        constexpr AssetId SceneDepthCopyFragId{0x0BC0737A4D1B42F7ULL};

        constexpr ImageUsage DepthUsage = ImageUsage::ColorAttachment | ImageUsage::Sampled;
    }

    Unique<RefractionGrab> RefractionGrab::Create(Context& context, AssetManager& assets)
    {
        return Unique<RefractionGrab>(new RefractionGrab(context, assets));
    }

    RefractionGrab::RefractionGrab(Context& context, AssetManager& assets)
        : m_Context(context),
          m_Chain(SceneColorChain::Create(context, assets, "SceneRenderer Refraction Scene"))
    {
        const auto LoadShader = [&](const AssetId id, const char* what) -> AssetHandle<Veng::Shader>
        {
            const AssetResult<AssetHandle<Veng::Shader>> result = assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(result.has_value(), "RefractionGrab: {} shader load failed: {}", what,
                      result.error().Detail);
            return *result;
        };

        const AssetHandle<Veng::Shader> vs = LoadShader(FullscreenVertId, "fullscreen vertex");
        const AssetHandle<Veng::Shader> sceneDepthCopyFs =
            LoadShader(SceneDepthCopyFragId, "scene-depth copy fragment");

        m_CopyLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Scene Depth Copy Layout",
                           .PushConstantRanges = {PushConstantRange::Of<SceneColorCopyPush>(
                               ShaderStage::Fragment)},
                       });
        m_DepthCopyPipeline = GraphicsPipeline::Create(
            m_Context,
            {
                .Name = "SceneRenderer Scene Depth Copy Pipeline",
                .ColorAttachments = {{.Format = Format::R32Sfloat}},
                .PipelineLayout = m_CopyLayout,
                .ShaderStages =
                    {
                        {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                        {.Stage = ShaderStage::Fragment, .Module = sceneDepthCopyFs.Get()->Module},
                    },
            });
    }

    RefractionGrab::~RefractionGrab()
    {
        m_Context.GetBindlessRegistry().Release(m_DepthHandle);
    }

    void RefractionGrab::Recreate(const SceneRendererSettings& settings, const bool depthCopy,
                                  const uvec2 extent)
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_DepthHandle);
        m_DepthHandle = {};

        // The opaque depth stands apart from the colour: a material may read it with no Refraction
        // setting, and the colour copy reads nothing of it.
        if (depthCopy)
        {
            m_DepthImage = Image::Create(m_Context, {
                                                        .Name = "SceneRenderer Refraction Depth",
                                                        .Extent = {extent.x, extent.y, 1},
                                                        .Format = Format::R32Sfloat,
                                                        .Usage = DepthUsage,
                                                    });
            m_DepthView = ImageView::Create(
                m_Context, {.Name = "SceneRenderer Refraction Depth View", .Image = m_DepthImage});
            m_DepthHandle = bindless.Register(m_DepthView);
        }
        else
        {
            m_DepthImage.reset();
            m_DepthView.reset();
        }

        // The pre-translucent scene color the copy pass fills each frame; translucent materials
        // sample it through the view block's SceneColor handles. With the blur on, it carries a
        // halving chain the copy's own level is the base of, so a material can read the scene behind
        // it at a level rather than only sharp.
        if (!settings.Refraction)
        {
            m_Chain->Release();
            return;
        }
        m_Chain->Recreate(extent,
                          settings.RefractionBlur ? SceneColorChain::BlurLevelsFor(extent) : 1u);
    }

    void RefractionGrab::DeclareDepthCopy(vector<Unique<ScenePass>>& passes,
                                          const ResourceId depthId, const TextureHandle depthHandle,
                                          const ResourceId depthCopyId, const SamplerHandle sampler,
                                          const uvec2 extent, const bool* sampled) const
    {
        passes.push_back(CreateUnique<SceneColorCopyScenePass>(
            m_Context,
            SceneColorCopyPassInfo{
                .Name = "Scene Depth Copy",
                .Pipeline = m_DepthCopyPipeline,
                .SourceId = depthId,
                .SourceHandle = depthHandle,
                .TargetId = depthCopyId,
                // Reverse-Z: the far plane, so a texel outside the copied sub-rect reads as
                // background rather than as an occluder.
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
                .Sampler = sampler,
                .Extent = extent,
                .Sampled = sampled,
            }));
    }

    void RefractionGrab::DeclareColorCopy(vector<Unique<ScenePass>>& passes,
                                          const ResourceId sourceId,
                                          const TextureHandle sourceHandle,
                                          const SamplerHandle sampler,
                                          const std::span<const ResourceId> mipIds,
                                          const bool* sampled) const
    {
        m_Chain->Declare(passes, "Scene Color", sourceId, sourceHandle, sampler, mipIds,
                         SceneColorChainRegion::RenderSubRect, sampled);
    }
}
