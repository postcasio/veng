#include "HalfResTranslucency.h"

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GBuffer.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>

#include "Passes/HalfResCompositeScenePass.h"
#include "Passes/HalfResDepthReduceScenePass.h"
#include "Passes/TranslucentScenePass.h"

namespace Veng::Renderer
{
    namespace
    {
        // The fullscreen vertex stage shared by the reduce and composite pipelines.
        constexpr AssetId FullscreenVertId{0xF46DD3C6F2AE0628ULL};
        // The half-res depth reduce fragment shader.
        constexpr AssetId HalfResDepthReduceFragId{0x8A1DD0C15A953B2DULL};
        // The half-res layer composite fragment shader.
        constexpr AssetId HalfResCompositeFragId{0xA5BEC069EF71BAB6ULL};

        // Linear float HDR format for the layer target, matching the lit scene color it
        // composites into.
        constexpr Format HdrFormat = Format::RGBA16Sfloat;
    }

    Unique<HalfResTranslucency> HalfResTranslucency::Create(Context& context, AssetManager& assets)
    {
        return Unique<HalfResTranslucency>(new HalfResTranslucency(context, assets));
    }

    HalfResTranslucency::HalfResTranslucency(Context& context, AssetManager& assets)
        : m_Context(context)
    {
        auto LoadShader = [&](const AssetId id, const char* what) -> AssetHandle<Veng::Shader>
        {
            const AssetResult<AssetHandle<Veng::Shader>> result = assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(result.has_value(), "HalfResTranslucency: {} shader load failed: {}", what,
                      result.error().Detail);
            return *result;
        };

        const AssetHandle<Veng::Shader> vs = LoadShader(FullscreenVertId, "fullscreen vertex");
        const AssetHandle<Veng::Shader> reduceFs =
            LoadShader(HalfResDepthReduceFragId, "half-res depth reduce fragment");
        const AssetHandle<Veng::Shader> compositeFs =
            LoadShader(HalfResCompositeFragId, "half-res composite fragment");

        m_ReduceLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Half-Res Depth Reduce Layout",
                           .PushConstantRanges = {PushConstantRange::Of<HalfResDepthReducePush>(
                               ShaderStage::Fragment)},
                       });
        // No color attachment: the fragment's whole output is SV_Depth into the half depth
        // target. Compare Always with writes on — every covered texel takes the reduced value.
        m_ReducePipeline = GraphicsPipeline::Create(
            m_Context,
            {
                .Name = "SceneRenderer Half-Res Depth Reduce",
                .DepthAttachmentFormat = GBuffer::DepthFormat,
                .PipelineLayout = m_ReduceLayout,
                .ShaderStages =
                    {
                        {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                        {.Stage = ShaderStage::Fragment, .Module = reduceFs.Get()->Module},
                    },
                .DepthTestEnable = true,
                .DepthWriteEnable = true,
                .DepthCompareOp = CompareOp::Always,
            });

        m_CompositeLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Half-Res Composite Layout",
                           .PushConstantRanges = {PushConstantRange::Of<HalfResCompositePush>(
                               ShaderStage::Fragment)},
                       });
        // The layer holds (premultiplied color, coverage), so the composite lays it under the
        // full-res translucents with the premultiplied blend rather than a straight one.
        m_CompositePipeline = GraphicsPipeline::Create(
            m_Context,
            {
                .Name = "SceneRenderer Half-Res Composite",
                .ColorAttachments = {{.Format = HdrFormat,
                                      .Blend =
                                          BlendState{
                                              .Enable = true,
                                              .SrcColorFactor = BlendFactor::One,
                                              .DstColorFactor = BlendFactor::OneMinusSrcAlpha,
                                              .ColorOp = BlendOp::Add,
                                              .SrcAlphaFactor = BlendFactor::One,
                                              .DstAlphaFactor = BlendFactor::OneMinusSrcAlpha,
                                              .AlphaOp = BlendOp::Add,
                                          }}},
                .PipelineLayout = m_CompositeLayout,
                .ShaderStages =
                    {
                        {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                        {.Stage = ShaderStage::Fragment, .Module = compositeFs.Get()->Module},
                    },
            });
    }

    HalfResTranslucency::~HalfResTranslucency()
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_LayerHandle);
        bindless.Release(m_DepthHandle);
    }

    void HalfResTranslucency::Recreate(const bool active, const uvec2 extent)
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_LayerHandle);
        bindless.Release(m_DepthHandle);
        m_LayerHandle = {};
        m_DepthHandle = {};

        if (!active)
        {
            m_LayerImage.reset();
            m_LayerView.reset();
            m_DepthImage.reset();
            m_DepthView.reset();
            return;
        }

        const uvec2 halfExtent = HalfResExtent(extent);
        m_LayerImage =
            Image::Create(m_Context, {
                                         .Name = "SceneRenderer Half-Res Translucent Layer",
                                         .Extent = {halfExtent.x, halfExtent.y, 1},
                                         .Format = HdrFormat,
                                         .Usage = ImageUsage::ColorAttachment | ImageUsage::Sampled,
                                     });
        m_LayerView =
            ImageView::Create(m_Context, {.Name = "SceneRenderer Half-Res Translucent Layer View",
                                          .Image = m_LayerImage});
        m_LayerHandle = bindless.Register(m_LayerView);

        m_DepthImage =
            Image::Create(m_Context, {
                                         .Name = "SceneRenderer Half-Res Translucent Depth",
                                         .Extent = {halfExtent.x, halfExtent.y, 1},
                                         .Format = GBuffer::DepthFormat,
                                         .Usage = ImageUsage::DepthAttachment | ImageUsage::Sampled,
                                     });
        m_DepthView =
            ImageView::Create(m_Context, {.Name = "SceneRenderer Half-Res Translucent Depth View",
                                          .Image = m_DepthImage});
        m_DepthHandle = bindless.Register(m_DepthView);
    }

    void HalfResTranslucency::Declare(vector<Unique<ScenePass>>& passes, const ResourceId layerId,
                                      const ResourceId halfDepthId, const ResourceId depthId,
                                      const TextureHandle depthHandle, const ResourceId targetId,
                                      const TranslucentDrawPlan* plan,
                                      const ResourceId sceneColorId, const ResourceId sceneDepthId,
                                      const uvec2 extent) const
    {
        passes.push_back(CreateUnique<HalfResDepthReduceScenePass>(
            m_Context, m_ReducePipeline, depthId, depthHandle, halfDepthId, plan));
        // The layer's own translucent pass: half viewport, transparent clear, no bloom mask —
        // the layer is composited into the lit color before the bloom bright-pass reads it, so
        // its glow rides the scene the way an unmasked full-res translucent's does.
        passes.push_back(CreateUnique<TranslucentScenePass>(
            m_Context, HalfResExtent(extent), plan, layerId, halfDepthId, sceneColorId,
            sceneDepthId, HdrFormat, ResourceId{}, Format::Undefined, /*halfResolution=*/true));
        passes.push_back(CreateUnique<HalfResCompositeScenePass>(
            m_Context, m_CompositePipeline, layerId, m_LayerHandle, halfDepthId, m_DepthHandle,
            depthId, depthHandle, targetId, plan));
    }
}
