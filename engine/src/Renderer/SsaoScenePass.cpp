#include "SsaoScenePass.h"

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/RenderGraph.h>

namespace Veng::Renderer
{
    namespace
    {
        // Single-channel unorm occlusion factor. Sampled alongside ColorAttachment so
        // RenderGraph::Compile's usage assertion passes.
        constexpr Format AoFormat = Format::R8Unorm;
        constexpr ImageUsage AoUsage = ImageUsage::ColorAttachment | ImageUsage::Sampled;

        // The SSAO fragment push block: g-buffer normal/depth slots, sampler,
        // view-constants index, and extent for noise tiling.
        // Matches ssao.frag PushConstants byte-for-byte (eight u32s).
        struct SsaoPushConstants
        {
            u32 NormalTexture;
            u32 DepthTexture;
            u32 Sampler;
            u32 ViewConstantsIndex;
            u32 ExtentX;
            u32 ExtentY;
            u32 Pad0;
            u32 Pad1;
        };
    }

    SsaoScenePass::SsaoScenePass(Context& context, Ref<GraphicsPipeline> pipeline,
                                 SamplerHandle samplerHandle, uvec2 extent)
        : m_Context(context), m_Pipeline(std::move(pipeline)),
          m_SamplerHandle(samplerHandle), m_Extent(extent)
    {
        CreateTarget();
    }

    SsaoScenePass::~SsaoScenePass()
    {
        // Release the bindless slot before the image retires.
        m_Context.GetBindlessRegistry().Release(m_AoHandle);
    }

    void SsaoScenePass::CreateTarget()
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_AoHandle);

        m_AoImage = Image::Create(m_Context, {
            .Name = "SceneRenderer SSAO",
            .Extent = {m_Extent.x, m_Extent.y, 1},
            .Format = AoFormat,
            .Usage = AoUsage,
        });
        m_AoView = ImageView::Create(m_Context, {.Name = "SceneRenderer SSAO View", .Image = m_AoImage});

        m_AoHandle = bindless.Register(m_AoView);
    }

    void SsaoScenePass::Resize(const uvec2 extent)
    {
        // Skip recreation when the extent is unchanged: Rebuild calls Resize on every
        // pass with the current extent, and recreating would invalidate the bindless
        // handle the renderer already read for the lighting pass.
        if (extent == m_Extent && m_AoView)
            return;
        m_Extent = extent;
        CreateTarget();
    }

    void SsaoScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        const TextureHandle normalHandle = io.NormalHandle;
        const TextureHandle depthHandle = io.DepthHandle;
        const SamplerHandle samplerHandle = m_SamplerHandle;
        const uvec2 extent = m_Extent;

        graph.AddPass("Scene SSAO")
            .Color({
                .Resource = io.Ssao,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{1.0f, 1.0f, 1.0f, 1.0f}, // 1 = unoccluded, the default for texels the kernel does not touch
            })
            .Sample(io.GBufferNormal)
            .Sample(io.GBufferDepth)
            .Execute([this, normalHandle, depthHandle, samplerHandle, extent](PassContext& inner)
            {
                CommandBuffer& cmd = inner.Cmd();
                const BindlessRegistry& registry = m_Context.GetBindlessRegistry();

                cmd.BindPipeline(m_Pipeline);
                cmd.SetViewport({0, 0}, extent);
                cmd.SetScissor({0, 0}, extent);
                registry.Bind(cmd);
                cmd.PushConstants(SsaoPushConstants{
                    .NormalTexture = normalHandle.Index,
                    .DepthTexture = depthHandle.Index,
                    .Sampler = samplerHandle.Index,
                    .ViewConstantsIndex = registry.GetCurrentViewConstantsIndex(),
                    .ExtentX = extent.x,
                    .ExtentY = extent.y,
                });
                cmd.DrawFullscreenTriangle();
            });
    }
}
