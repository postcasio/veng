#include "TaaResolve.h"

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

#include "Passes/TaaScenePass.h"

namespace Veng::Renderer
{
    namespace
    {
        // The shared fullscreen vertex stage and the TAA resolve / history-copy fragment shaders.
        constexpr AssetId FullscreenVertId{0xF46DD3C6F2AE0628ULL};
        constexpr AssetId TaaResolveFragId{0xF277BB65AEDAC33EULL};
        constexpr AssetId TaaHistoryCopyFragId{0x07F31C1EC98A29BFULL};

        // Linear float HDR format for the lit/history targets — the format the rest of the tail
        // samples, so the resolve round-trips through it.
        constexpr Format HdrFormat = Format::RGBA16Sfloat;
        constexpr ImageUsage HdrUsage = ImageUsage::ColorAttachment | ImageUsage::Sampled;
    }

    Unique<TaaResolve> TaaResolve::Create(Context& context, AssetManager& assets)
    {
        return Unique<TaaResolve>(new TaaResolve(context, assets));
    }

    TaaResolve::TaaResolve(Context& context, AssetManager& assets) : m_Context(context)
    {
        auto LoadShader = [&](const AssetId id, const char* what) -> AssetHandle<Veng::Shader>
        {
            const AssetResult<AssetHandle<Veng::Shader>> result = assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(result.has_value(), "TaaResolve: {} shader load failed: {}", what,
                      result.error().Detail);
            return *result;
        };

        const AssetHandle<Veng::Shader> vs = LoadShader(FullscreenVertId, "fullscreen vertex");
        const AssetHandle<Veng::Shader> resolveFs =
            LoadShader(TaaResolveFragId, "TAA resolve fragment");
        const AssetHandle<Veng::Shader> copyFs =
            LoadShader(TaaHistoryCopyFragId, "TAA history-copy fragment");

        auto MakePipeline = [&](const char* name, const Ref<PipelineLayout>& layout,
                                const AssetHandle<Veng::Shader>& fs) -> Ref<GraphicsPipeline>
        {
            return GraphicsPipeline::Create(
                m_Context, {
                               .Name = name,
                               .ColorAttachments = {{.Format = HdrFormat}},
                               .PipelineLayout = layout,
                               .ShaderStages =
                                   {
                                       {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                                       {.Stage = ShaderStage::Fragment, .Module = fs.Get()->Module},
                                   },
                           });
        };

        // TAA resolve writes the HDR target (the resolved color the rest of the chain reads);
        // its push carries the bindless slots, view-constants region, history flag, and extent.
        m_ResolveLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer TAA Resolve Layout",
                           .PushConstantRanges = {PushConstantRange::Of<TaaResolvePush>(
                               ShaderStage::Fragment)},
                       });
        m_ResolvePipeline =
            MakePipeline("SceneRenderer TAA Resolve Pipeline", m_ResolveLayout, resolveFs);

        // TAA history-copy writes the persisted history target (HDR, unclamped).
        m_CopyLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "SceneRenderer TAA History Copy Layout",
                .PushConstantRanges = {PushConstantRange::Of<TaaCopyPush>(ShaderStage::Fragment)},
            });
        m_CopyPipeline =
            MakePipeline("SceneRenderer TAA History Copy Pipeline", m_CopyLayout, copyFs);
    }

    TaaResolve::~TaaResolve()
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_LitHandle);
        bindless.Release(m_HistoryHandle);
    }

    void TaaResolve::Resize(const uvec2 extent, const bool enabled)
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();

        // Recreating (or releasing) invalidates the persisted history, so the next resolve
        // must ignore it until a frame repopulates it.
        m_HistoryReset = true;

        bindless.Release(m_LitHandle);
        bindless.Release(m_HistoryHandle);
        m_LitHandle = {};
        m_HistoryHandle = {};

        if (enabled)
        {
            m_LitImage = Image::Create(m_Context, {
                                                      .Name = "SceneRenderer Lit",
                                                      .Extent = {extent.x, extent.y, 1},
                                                      .Format = HdrFormat,
                                                      .Usage = HdrUsage,
                                                  });
            m_LitView = ImageView::Create(m_Context,
                                          {.Name = "SceneRenderer Lit View", .Image = m_LitImage});
            m_LitHandle = bindless.Register(m_LitView);

            m_HistoryImage = Image::Create(m_Context, {
                                                          .Name = "SceneRenderer TAA History",
                                                          .Extent = {extent.x, extent.y, 1},
                                                          .Format = HdrFormat,
                                                          .Usage = HdrUsage,
                                                      });
            m_HistoryView = ImageView::Create(
                m_Context, {.Name = "SceneRenderer TAA History View", .Image = m_HistoryImage});
            m_HistoryHandle = bindless.Register(m_HistoryView);
        }
        else
        {
            // Drop the resolve targets when TAA is off so the memory is not held for an unused path.
            m_LitImage.reset();
            m_LitView.reset();
            m_HistoryImage.reset();
            m_HistoryView.reset();
        }
    }
}
