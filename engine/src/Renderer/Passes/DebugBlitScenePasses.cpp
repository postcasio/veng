#include "DebugBlitScenePasses.h"

#include <Veng/Assert.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DofTile.h>
#include <Veng/Renderer/GraphicsPipeline.h>

namespace Veng::Renderer
{
    void FullscreenBlitScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        const ResourceId sourceId = SourceId(io);
        const TextureHandle textureHandle = SourceHandle(io);
        const SamplerHandle samplerHandle = io.SamplerHandle;

        graph.AddPass("Debug Blit")
            .Color({
                .Resource = io.Output,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            })
            .Sample(sourceId)
            .Execute(
                [this, textureHandle, samplerHandle](PassContext& inner)
                {
                    CommandBuffer& cmd = inner.Cmd();
                    cmd.BindPipeline(m_Pipeline);
                    cmd.SetViewport({0, 0}, m_Extent);
                    cmd.SetScissor({0, 0}, m_Extent);
                    m_Context.GetBindlessRegistry().Bind(cmd);
                    cmd.PushConstants(BlitPushConstants{
                        .Texture = textureHandle.Index,
                        .Sampler = samplerHandle.Index,
                    });
                    cmd.DrawFullscreenTriangle();
                });
    }

    ResourceId FullscreenBlitScenePass::SourceId(const PassIO& io) const
    {
        switch (m_Source)
        {
        case Source::Albedo:
            return io.GBufferAlbedo;
        case Source::Normal:
            return io.GBufferNormal;
        case Source::Depth:
            return io.GBufferDepth;
        case Source::Ao:
            return io.Ssao;
        case Source::Bloom:
            return io.BloomMip0;
        case Source::MotionVectors:
            return io.Velocity;
        case Source::Reflections:
            return io.SsrReflection;
        case Source::Emissive:
            return io.GBufferEmissive;
        }
        VE_ASSERT(false, "FullscreenBlitScenePass: unmapped Source");
    }

    TextureHandle FullscreenBlitScenePass::SourceHandle(const PassIO& io) const
    {
        switch (m_Source)
        {
        case Source::Albedo:
            return io.AlbedoHandle;
        case Source::Normal:
            return io.NormalHandle;
        case Source::Depth:
            return io.DepthHandle;
        case Source::Ao:
            return io.SsaoHandle;
        case Source::Bloom:
            return io.BloomMip0Handle;
        case Source::MotionVectors:
            return io.VelocityHandle;
        case Source::Reflections:
            return io.SsrReflectionHandle;
        case Source::Emissive:
            return io.EmissiveHandle;
        }
        VE_ASSERT(false, "FullscreenBlitScenePass: unmapped Source");
    }

    void OrmBlitScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        const ResourceId ormId = io.GBufferOrm;
        const TextureHandle ormHandle = io.OrmHandle;
        const SamplerHandle samplerHandle = io.SamplerHandle;
        const u32 channel = m_Channel;

        graph.AddPass("ORM Debug Blit")
            .Color({
                .Resource = io.Output,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            })
            .Sample(ormId)
            .Execute(
                [this, ormHandle, samplerHandle, channel](PassContext& inner)
                {
                    CommandBuffer& cmd = inner.Cmd();
                    cmd.BindPipeline(m_Pipeline);
                    cmd.SetViewport({0, 0}, m_Extent);
                    cmd.SetScissor({0, 0}, m_Extent);
                    m_Context.GetBindlessRegistry().Bind(cmd);
                    cmd.PushConstants(OrmBlitPushConstants{
                        .Texture = ormHandle.Index,
                        .Sampler = samplerHandle.Index,
                        .Channel = channel,
                    });
                    cmd.DrawFullscreenTriangle();
                });
    }

    void CocBlitScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        const TextureHandle cocHandle = io.DofCocHandle;
        const SamplerHandle samplerHandle = io.SamplerHandle;

        graph.AddPass("CoC Debug Blit")
            .Color({
                .Resource = io.Output,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            })
            .Sample(io.DofCoc)
            .Execute(
                [this, cocHandle, samplerHandle](PassContext& inner)
                {
                    const ScenePassContext ctx = Wrap(inner);
                    CommandBuffer& cmd = ctx.Cmd();
                    cmd.BindPipeline(m_Pipeline);
                    cmd.SetViewport({0, 0}, m_Extent);
                    cmd.SetScissor({0, 0}, m_Extent);
                    m_Context.GetBindlessRegistry().Bind(cmd);
                    cmd.PushConstants(CocBlitPushConstants{
                        .Texture = cocHandle.Index,
                        .Sampler = samplerHandle.Index,
                        .MaxCoc = ClampDofMaxCoc(ctx.View().DofMaxCoc),
                    });
                    cmd.DrawFullscreenTriangle();
                });
    }

    void ShadowBlitScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        const Ref<DescriptorSet> shadowSet = m_ShadowSet;
        const ResourceId sampleId =
            m_Source == Source::Punctual ? io.PunctualShadowMap : io.ShadowMap;

        graph.AddPass("Shadow Debug Blit")
            .Color({
                .Resource = io.Output,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            })
            .Sample(sampleId)
            .Execute(
                [this, shadowSet](PassContext& inner)
                {
                    CommandBuffer& cmd = inner.Cmd();
                    cmd.BindPipeline(m_Pipeline);
                    cmd.SetViewport({0, 0}, m_Extent);
                    cmd.SetScissor({0, 0}, m_Extent);
                    m_Context.GetBindlessRegistry().Bind(cmd);
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {shadowSet},
                        .FirstSet = 1,
                        .PipelineBindPoint = PipelineBindPoint::Graphics,
                    });
                    cmd.DrawFullscreenTriangle();
                });
    }
}
