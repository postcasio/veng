#include "TranslucentScenePass.h"

#include <fmt/format.h>

#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/GBuffer.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/PipelineLayout.h>

namespace Veng::Renderer
{
    void TranslucentScenePass::Declare(RenderGraph& graph, const PassIO& /*io*/)
    {
        RenderGraph::PassBuilder builder = graph.AddPass("Scene Translucent");
        builder
            .Color({
                // Alpha-blend into the lit scene color (blend enabled on the pipeline).
                .Resource = m_TargetId,
                .Load = LoadOp::Load,
                .Store = StoreOp::Store,
            })
            .Depth({
                // The opaque depth bound read-only (the pipeline disables depth writes):
                // translucents depth-test against the resolved opaque scene so they are
                // occluded correctly, without occluding one another.
                .Resource = m_DepthId,
                .Load = LoadOp::Load,
                .Store = StoreOp::Store,
            });
        if (m_SceneColorId.IsValid())
        {
            builder.Sample(m_SceneColorId);
        }
        if (m_SceneDepthId.IsValid())
        {
            builder.Sample(m_SceneDepthId);
        }
        builder.Execute([this](PassContext& inner) { Record(Wrap(inner)); });
    }

    const Ref<GraphicsPipeline>&
    TranslucentScenePass::PipelineFor(const MaterialInstance& material) const
    {
        const Material* parent = material.GetParent().Get();
        const auto it = m_Pipelines.find(parent);
        if (it != m_Pipelines.end())
        {
            return it->second;
        }

        Ref<GraphicsPipeline> pipeline = GraphicsPipeline::Create(
            m_Context,
            {
                .Name = fmt::format("Translucent Pipeline ({})", material.GetName()),
                .ColorAttachments = {{.Format = m_TargetFormat, .Blend = BlendState::AlphaBlend()}},
                .DepthAttachmentFormat = GBuffer::DepthFormat,
                .VertexBufferLayout = Mesh::CanonicalLayout(),
                // The surface vertex stage reads the per-draw candidate id as an
                // instance-rate attribute on binding 1 (fetched at firstInstance).
                .InstanceCandidateId = true,
                .PipelineLayout = material.GetPipelineLayout(),
                .ShaderStages =
                    {
                        {.Stage = ShaderStage::Vertex, .Module = material.GetVertexModule()},
                        {.Stage = ShaderStage::Fragment, .Module = material.GetFragmentModule()},
                    },
                // The material's authored cull mode (Back when unauthored, the opaque
                // surface convention) — a two-sided translucent surface authors None.
                .CullMode = parent->GetCullMode(),
                .DepthTestEnable = true,
                .DepthWriteEnable = false,
                // Reverse-Z: a nearer fragment has larger depth.
                .DepthCompareOp = CompareOp::GreaterOrEqual,
            });

        return m_Pipelines.emplace(parent, std::move(pipeline)).first->second;
    }

    void TranslucentScenePass::Record(const ScenePassContext& ctx) const
    {
        CommandBuffer& cmd = ctx.Cmd();
        const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
        const TranslucentDrawPlan& plan = *m_Plan;

        const uvec2 renderExtent = ctx.View().RenderExtent;
        cmd.SetViewport({0, 0}, renderExtent);
        cmd.SetScissor({0, 0}, renderExtent);

        if (plan.Draws.empty())
        {
            return;
        }

        // The instance-rate candidate-id buffer (binding 1) is bound once; each draw's
        // firstInstance selects the candidate id that indexes DrawData.
        cmd.GetNative().CommandBuffer.bindVertexBuffers(1, GetVkBuffer(*plan.CandidateIdBuffer),
                                                        {0});

        // Back-to-front: bind each draw's material pipeline (rebound only on a change, since
        // sorting interleaves materials by depth), the bindless registry, and the shared
        // DrawData set, then the mesh buffers, then draw the submesh. The surface push is
        // rebound with the pipeline (its layout is per-material). Translucent materials push
        // no selector (they read it from DrawData), so Material::Bind only binds the
        // pipeline; the selector rides each draw's DrawData record via the candidate id.
        const GraphicsPipeline* lastPipeline = nullptr;
        const Mesh* lastMesh = nullptr;
        for (const TranslucentDraw& draw : plan.Draws)
        {
            const Ref<GraphicsPipeline>& pipeline = PipelineFor(*draw.Material);
            if (pipeline.get() != lastPipeline)
            {
                cmd.BindPipeline(pipeline);
                registry.Bind(cmd);
                cmd.BindDescriptorSets(DescriptorSetBindInfo{
                    .Sets = {plan.DrawDataSet},
                    .FirstSet = 1,
                    .PipelineBindPoint = PipelineBindPoint::Graphics,
                });
                cmd.PushConstants(plan.Push);
                lastPipeline = pipeline.get();
                lastMesh = nullptr;
            }
            if (draw.SourceMesh != lastMesh)
            {
                cmd.BindVertexBuffer(draw.SourceMesh->GetVertexBuffer());
                cmd.BindIndexBuffer(draw.SourceMesh->GetIndexBuffer());
                lastMesh = draw.SourceMesh;
            }
            cmd.DrawIndexed(draw.IndexCount, 1, draw.FirstIndex, 0, draw.CandidateId);
        }
    }
}
