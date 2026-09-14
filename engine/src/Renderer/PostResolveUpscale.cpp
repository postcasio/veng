#include "PostResolveUpscale.h"

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/Types.h>

#include "Passes/SceneUpscaleScenePass.h"
#include "SceneRendererIds.h"

namespace Veng::Renderer
{
    namespace
    {
        // The engine core pack's spatial scene-color upscale fragment shader.
        constexpr AssetId SceneUpscaleFragId{0x1C0CB4545070CC5FULL};
    }

    Unique<PostResolveUpscale> PostResolveUpscale::Create(Context& context, AssetManager& assets)
    {
        return Unique<PostResolveUpscale>(new PostResolveUpscale(context, assets));
    }

    PostResolveUpscale::PostResolveUpscale(Context& context, AssetManager& assets)
        : m_Context(context)
    {
        auto LoadShader = [&](const AssetId id, const char* what) -> AssetHandle<Veng::Shader>
        {
            const AssetResult<AssetHandle<Veng::Shader>> result = assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(result.has_value(), "PostResolveUpscale: {} shader load failed: {}", what,
                      result.error().Detail);
            return *result;
        };

        const AssetHandle<Veng::Shader> vs = LoadShader(FullscreenVertId, "fullscreen vertex");
        const AssetHandle<Veng::Shader> fs =
            LoadShader(SceneUpscaleFragId, "scene upscale fragment");

        m_Layout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Scene Upscale Layout",
                           .PushConstantRanges = {PushConstantRange::Of<SceneUpscalePush>(
                               ShaderStage::Fragment)},
                       });
        m_Pipeline = GraphicsPipeline::Create(
            m_Context, {
                           .Name = "SceneRenderer Scene Upscale Pipeline",
                           .ColorAttachments = {{.Format = HdrFormat}},
                           .PipelineLayout = m_Layout,
                           .ShaderStages =
                               {
                                   {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                                   {.Stage = ShaderStage::Fragment, .Module = fs.Get()->Module},
                               },
                       });
        // The same shader pair: the fragment emits a float4 whose red channel is the resampled
        // source, which is the whole of a single-channel mask attachment.
        m_MaskPipeline = GraphicsPipeline::Create(
            m_Context, {
                           .Name = "SceneRenderer Bloom Mask Upscale Pipeline",
                           .ColorAttachments = {{.Format = BloomMaskFormat}},
                           .PipelineLayout = m_Layout,
                           .ShaderStages =
                               {
                                   {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                                   {.Stage = ShaderStage::Fragment, .Module = fs.Get()->Module},
                               },
                       });
    }

    PostResolveUpscale::~PostResolveUpscale()
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_SceneHandle);
        bindless.Release(m_MaskHandle);
    }

    void PostResolveUpscale::Resize(const uvec2 extent, const bool enabled)
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_SceneHandle);
        m_SceneHandle = {};

        if (!enabled)
        {
            m_SceneImage.reset();
            m_SceneView.reset();
            return;
        }

        m_SceneImage = Image::Create(m_Context, {
                                                    .Name = "SceneRenderer Promoted Scene",
                                                    .Extent = {extent.x, extent.y, 1},
                                                    .Format = HdrFormat,
                                                    .Usage = HdrUsage,
                                                });
        m_SceneView = ImageView::Create(
            m_Context, {.Name = "SceneRenderer Promoted Scene View", .Image = m_SceneImage});
        m_SceneHandle = bindless.Register(m_SceneView);
    }

    void PostResolveUpscale::ResizeMask(const uvec2 extent, const bool enabled)
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_MaskHandle);
        m_MaskHandle = {};

        if (!enabled)
        {
            m_MaskImage.reset();
            m_MaskView.reset();
            return;
        }

        m_MaskImage = Image::Create(m_Context, {
                                                   .Name = "SceneRenderer Promoted Bloom Mask",
                                                   .Extent = {extent.x, extent.y, 1},
                                                   .Format = BloomMaskFormat,
                                                   .Usage = BloomMaskUsage,
                                               });
        m_MaskView = ImageView::Create(
            m_Context, {.Name = "SceneRenderer Promoted Bloom Mask View", .Image = m_MaskImage});
        m_MaskHandle = bindless.Register(m_MaskView);
    }
}
