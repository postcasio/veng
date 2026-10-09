#include "PortraitRenderer.h"

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/SceneRenderer.h>

namespace Veng::Renderer
{
    namespace
    {
        // The shared fullscreen vertex stage, and the coverage fragment (portrait_coverage.frag).
        constexpr AssetId FullscreenVertId{0xF46DD3C6F2AE0628ULL};
        constexpr AssetId CoverageFragId{0xAF2455E4EE50909BULL};

        // The coverage push block, matching portrait_coverage.frag PushConstants.
        struct CoveragePush
        {
            u32 Color;
            u32 Depth;
        };

        /// @brief The lean battery set a portrait renders with: what a UI thumbnail shows, no more.
        SceneRendererSettings PortraitSettings(const PortraitOutput output)
        {
            SceneRendererSettings settings;
            settings.Path = output == PortraitOutput::GeometryDepthNormal
                                ? RenderPath::GeometryDepthNormal
                                : RenderPath::Shaded;
            settings.Bloom = false;
            settings.Shadows = false;
            settings.PunctualShadows = false;
            settings.AO = false;
            settings.SSR = false;
            settings.PostProcessEffects = false;
            settings.AntiAliasing = AntiAliasingMode::None;
            return settings;
        }

        AssetHandle<Veng::Shader> LoadShader(AssetManager& assets, const AssetId id,
                                             const char* what)
        {
            const AssetResult<AssetHandle<Veng::Shader>> result = assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(result.has_value(), "PortraitRenderer: {} shader load failed: {}", what,
                      result.error().Detail);
            return *result;
        }
    }

    Unique<PortraitRenderer> CreatePortraitRenderer(Context& context, AssetManager& assets,
                                                    const PortraitRendererConfig& config)
    {
        return CreateUnique<PortraitRenderer>(context, assets, config);
    }

    PortraitRenderer::PortraitRenderer(Context& context, AssetManager& assets,
                                       const PortraitRendererConfig& config)
        : m_Context(context), m_Config(config)
    {
        VE_ASSERT(config.Extent.x > 0 && config.Extent.y > 0,
                  "PortraitRenderer: Extent must be positive (got {}x{})", config.Extent.x,
                  config.Extent.y);

        m_Renderer = SceneRenderer::Create({
            .Context = context,
            .Assets = assets,
            .OutputFormat = context.GetOutputFormat(),
            .Extent = config.Extent,
            .Settings = PortraitSettings(config.Output),
        });

        BindlessRegistry& bindless = context.GetBindlessRegistry();
        m_Sampler = bindless
                        .AcquireSampler({
                            .Name = "ModelPortrait Sampler",
                            .MagFilter = Filter::Linear,
                            .MinFilter = Filter::Linear,
                            .AddressModeU = AddressMode::ClampToEdge,
                            .AddressModeV = AddressMode::ClampToEdge,
                            .AddressModeW = AddressMode::ClampToEdge,
                        })
                        .Handle;

        if (config.Output != PortraitOutput::Shaded)
        {
            return;
        }

        // The renderer never resizes or reconfigures, so one registration of its output holds.
        m_OutputHandle = bindless.Register(m_Renderer->GetOutput());

        const Format format = context.GetOutputFormat();
        m_Color = Image::Create(context, {
                                             .Name = "ModelPortrait Color",
                                             .Extent = {config.Extent.x, config.Extent.y, 1},
                                             .Format = format,
                                             .Usage = ImageUsage::ColorAttachment |
                                                      ImageUsage::Sampled | ImageUsage::TransferSrc,
                                         });
        m_ColorView =
            ImageView::Create(context, {.Name = "ModelPortrait Color View", .Image = m_Color});
        m_ColorHandle = bindless.Register(m_ColorView);

        const AssetHandle<Veng::Shader> vs =
            LoadShader(assets, FullscreenVertId, "fullscreen vertex");
        const AssetHandle<Veng::Shader> fs =
            LoadShader(assets, CoverageFragId, "coverage fragment");
        m_CoverageLayout = PipelineLayout::Create(
            context,
            {
                .Name = "ModelPortrait Coverage Layout",
                .PushConstantRanges = {PushConstantRange::Of<CoveragePush>(ShaderStage::Fragment)},
            });
        m_CoveragePipeline = GraphicsPipeline::Create(
            context, {
                         .Name = "ModelPortrait Coverage Pipeline",
                         .ColorAttachments = {{.Format = format}},
                         .PipelineLayout = m_CoverageLayout,
                         .ShaderStages =
                             {
                                 {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                                 {.Stage = ShaderStage::Fragment, .Module = fs.Get()->Module},
                             },
                     });
    }

    PortraitRenderer::~PortraitRenderer()
    {
        // The sampler is the registry's shared one and is never released (see AcquireSampler).
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        if (m_OutputHandle.IsValid())
        {
            bindless.Release(m_OutputHandle);
        }
        if (m_ColorHandle.IsValid())
        {
            bindless.Release(m_ColorHandle);
        }
    }

    TextureHandle PortraitRenderer::GetDepthHandle() const
    {
        return m_Config.Output == PortraitOutput::GeometryDepthNormal ? m_Renderer->GetDepthHandle()
                                                                      : TextureHandle{};
    }

    TextureHandle PortraitRenderer::GetNormalHandle() const
    {
        return m_Config.Output == PortraitOutput::GeometryDepthNormal
                   ? m_Renderer->GetNormalHandle()
                   : TextureHandle{};
    }

    void PortraitRenderer::ResetForReuse()
    {
        m_Renderer->ReleaseScene();
        m_Rendered = false;
    }

    void PortraitRenderer::Render(CommandBuffer& cmd, const SceneView& view)
    {
        m_Renderer->Execute(cmd, view);
        m_Rendered = true;

        if (m_Config.Output == PortraitOutput::GeometryDepthNormal)
        {
            cmd.PrepareForAccess(m_Renderer->GetDepthView(), AccessKind::SampleGraphics);
            cmd.PrepareForAccess(m_Renderer->GetNormalView(), AccessKind::SampleGraphics);
            return;
        }

        // The tonemap leaves alpha opaque, so coverage is rebuilt from depth: a portrait is drawn
        // over a UI's own background, and only the model may cover it.
        cmd.PrepareForAccess(m_Renderer->GetOutput(), AccessKind::SampleGraphics);
        cmd.PrepareForAccess(m_Renderer->GetDepthView(), AccessKind::SampleGraphics);
        cmd.PrepareForAccess(m_ColorView, AccessKind::ColorAttachment);
        cmd.BeginRendering({
            .Extent = m_Config.Extent,
            .ColorAttachments = {{
                .ImageView = m_ColorView,
                .LoadOp = LoadOp::DontCare,
                .StoreOp = StoreOp::Store,
            }},
        });
        cmd.BindPipeline(m_CoveragePipeline);
        cmd.SetViewport({0, 0}, m_Config.Extent);
        cmd.SetScissor({0, 0}, m_Config.Extent);
        m_Context.GetBindlessRegistry().Bind(cmd);
        cmd.PushConstants(CoveragePush{
            .Color = m_OutputHandle.Index,
            .Depth = m_Renderer->GetDepthHandle().Index,
        });
        cmd.DrawFullscreenTriangle();
        cmd.EndRendering();
        cmd.PrepareForAccess(m_ColorView, AccessKind::SampleGraphics);
    }
}
