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
        // The refraction scene-color copy fragment shader.
        constexpr AssetId SceneColorCopyFragId{0xBE7002B7B8E9BE5AULL};

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
    }

    RefractionGrab::~RefractionGrab()
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_SceneHandle);
        bindless.Release(m_DepthHandle);
    }

    void RefractionGrab::Recreate(const SceneRendererSettings& settings, const uvec2 extent)
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_SceneHandle);
        bindless.Release(m_DepthHandle);
        m_SceneHandle = {};
        m_DepthHandle = {};

        if (!settings.Refraction)
        {
            m_SceneImage.reset();
            m_SceneView.reset();
            m_DepthImage.reset();
            m_DepthView.reset();
            return;
        }

        // The pre-translucent scene color and opaque depth the copy pass fills each frame;
        // translucent materials sample them through the view block's SceneColor handles.
        m_SceneImage = Image::Create(m_Context, {
                                                    .Name = "SceneRenderer Refraction Scene",
                                                    .Extent = {extent.x, extent.y, 1},
                                                    .Format = HdrFormat,
                                                    .Usage = HdrUsage,
                                                });
        m_SceneView = ImageView::Create(
            m_Context, {.Name = "SceneRenderer Refraction Scene View", .Image = m_SceneImage});
        m_SceneHandle = bindless.Register(m_SceneView);

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
                                 const uvec2 extent) const
    {
        passes.push_back(CreateUnique<SceneColorCopyScenePass>(
            m_Context, m_CopyPipeline, sourceId, sourceHandle, depthId, depthHandle, copyId,
            depthCopyId, sampler, extent));
    }
}
