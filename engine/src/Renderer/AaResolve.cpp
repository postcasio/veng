#include "AaResolve.h"

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

#include "Passes/AaScenePasses.h"
#include "SceneRendererIds.h"

namespace Veng::Renderer
{
    namespace
    {
        // The engine core pack's spatial-AA fragment shaders.
        constexpr AssetId FxaaFragId{0x4ADEE68A7BED1025ULL};
        constexpr AssetId Cmaa2EdgeFragId{0x54767E7678A44ADDULL};
        constexpr AssetId Cmaa2ApplyFragId{0x8C7CE6BB3884EBABULL};

        // The edge map: two-channel unorm (R = right edge, G = bottom edge), full extent.
        constexpr Format EdgeFormat = Format::RG8Unorm;
    }

    Unique<AaResolve> AaResolve::Create(Context& context, AssetManager& assets,
                                        const Format outputFormat)
    {
        return Unique<AaResolve>(new AaResolve(context, assets, outputFormat));
    }

    AaResolve::AaResolve(Context& context, AssetManager& assets, const Format outputFormat)
        : m_Context(context), m_OutputFormat(outputFormat)
    {
        auto LoadShader = [&](const AssetId id, const char* what) -> AssetHandle<Veng::Shader>
        {
            const AssetResult<AssetHandle<Veng::Shader>> result = assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(result.has_value(), "AaResolve: {} shader load failed: {}", what,
                      result.error().Detail);
            return *result;
        };

        const AssetHandle<Veng::Shader> vs = LoadShader(FullscreenVertId, "fullscreen vertex");
        const AssetHandle<Veng::Shader> fxaaFs = LoadShader(FxaaFragId, "FXAA fragment");
        const AssetHandle<Veng::Shader> edgeFs = LoadShader(Cmaa2EdgeFragId, "CMAA2 edge fragment");
        const AssetHandle<Veng::Shader> applyFs =
            LoadShader(Cmaa2ApplyFragId, "CMAA2 apply fragment");

        auto MakePipeline = [&](const char* name, const Ref<PipelineLayout>& layout,
                                const AssetHandle<Veng::Shader>& fs,
                                const Format format) -> Ref<GraphicsPipeline>
        {
            return GraphicsPipeline::Create(
                m_Context, {
                               .Name = name,
                               .ColorAttachments = {{.Format = format}},
                               .PipelineLayout = layout,
                               .ShaderStages =
                                   {
                                       {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                                       {.Stage = ShaderStage::Fragment, .Module = fs.Get()->Module},
                                   },
                           });
        };

        // FXAA and the CMAA2 edge pass share the texture + sampler + reciprocal-extent push block.
        m_FxaaLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "SceneRenderer Spatial AA Layout",
                .PushConstantRanges = {PushConstantRange::Of<FxaaPush>(ShaderStage::Fragment)},
            });
        m_FxaaPipeline =
            MakePipeline("SceneRenderer FXAA Pipeline", m_FxaaLayout, fxaaFs, m_OutputFormat);

        // CMAA2 edge pass: same push block, writes the RG8 edge map.
        m_Cmaa2EdgePipeline =
            MakePipeline("SceneRenderer CMAA2 Edge Pipeline", m_FxaaLayout, edgeFs, EdgeFormat);

        // CMAA2 apply pass: reads the color + edge maps, writes the output.
        m_Cmaa2ApplyLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer CMAA2 Apply Layout",
                           .PushConstantRanges = {PushConstantRange::Of<Cmaa2ApplyPush>(
                               ShaderStage::Fragment)},
                       });
        m_Cmaa2ApplyPipeline = MakePipeline("SceneRenderer CMAA2 Apply Pipeline",
                                            m_Cmaa2ApplyLayout, applyFs, m_OutputFormat);
    }

    AaResolve::~AaResolve()
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_InputHandle);
        bindless.Release(m_EdgeHandle);
    }

    void AaResolve::Resize(const uvec2 extent, const AntiAliasingMode mode)
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_InputHandle);
        bindless.Release(m_EdgeHandle);
        m_InputHandle = {};
        m_EdgeHandle = {};

        // The intermediate exists only under a post-tonemap mode; the temporal path resolves in HDR
        // and never routes through it.
        const bool active = mode == AntiAliasingMode::FXAA || mode == AntiAliasingMode::CMAA2;
        if (active)
        {
            m_InputImage = Image::Create(
                m_Context, {
                               .Name = "SceneRenderer AA Input",
                               .Extent = {extent.x, extent.y, 1},
                               .Format = m_OutputFormat,
                               .Usage = ImageUsage::ColorAttachment | ImageUsage::Sampled,
                           });
            m_InputView = ImageView::Create(
                m_Context, {.Name = "SceneRenderer AA Input View", .Image = m_InputImage});
            m_InputHandle = bindless.Register(m_InputView);
        }
        else
        {
            m_InputImage.reset();
            m_InputView.reset();
        }

        // The edge map is CMAA2's alone.
        if (mode == AntiAliasingMode::CMAA2)
        {
            m_EdgeImage = Image::Create(
                m_Context, {
                               .Name = "SceneRenderer CMAA2 Edges",
                               .Extent = {extent.x, extent.y, 1},
                               .Format = EdgeFormat,
                               .Usage = ImageUsage::ColorAttachment | ImageUsage::Sampled,
                           });
            m_EdgeView = ImageView::Create(
                m_Context, {.Name = "SceneRenderer CMAA2 Edges View", .Image = m_EdgeImage});
            m_EdgeHandle = bindless.Register(m_EdgeView);
        }
        else
        {
            m_EdgeImage.reset();
            m_EdgeView.reset();
        }
    }
}
