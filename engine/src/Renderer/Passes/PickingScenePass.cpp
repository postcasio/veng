#include "PickingScenePass.h"

#include <Veng/Asset/Mesh.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Native.h>

namespace Veng::Renderer
{
    void PickingScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        (void)io;
        RenderGraph::PassBuilder builder = graph.AddPass("Scene Picking");
        builder
            .Color({
                .Resource = m_EntityIdId,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                // Cleared to NoEntityId (0): any texel no surface covers reads back as
                // background. ClearColor's float channels carry the bit pattern; 0 maps
                // to the integer 0 the readback compares against.
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
            })
            .Depth({
                .Resource = m_DepthId,
                .Load = LoadOp::Clear,
                // Stored so the billboard id pass can depth-test against the mesh depth
                // (discarding an icon behind geometry) before the target is discarded.
                .Store = StoreOp::Store,
                // Reverse-Z: the far plane is 0.
                .Clear = ClearDepth{.Depth = 0.0f, .Stencil = 0},
            });
        builder.Execute([this](PassContext& inner) { Record(Wrap(inner)); });
    }

    void PickingScenePass::Record(const ScenePassContext& ctx) const
    {
        CommandBuffer& cmd = ctx.Cmd();
        const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
        const GBufferDrawPlan& plan = *m_Plan;

        const uvec2 renderExtent = ctx.View().RenderExtent;
        cmd.SetViewport({0, 0}, renderExtent);
        cmd.SetScissor({0, 0}, renderExtent);

        // The id-writing pipelines are built lazily on the first Execute a surface material
        // is available; until then (or with no drawable geometry) the pass clears the target
        // and writes nothing — background everywhere.
        const Ref<GraphicsPipeline>& staticPipeline = *m_StaticPipeline;
        const Ref<GraphicsPipeline>& skinnedPipeline = *m_SkinnedPipeline;

        if (plan.Slots.empty() && plan.SkinnedSlots.empty())
        {
            return;
        }
        if (!plan.Slots.empty() || !plan.SkinnedSlots.empty())
        {
            cmd.GetNative().CommandBuffer.bindVertexBuffers(1, GetVkBuffer(*plan.CandidateIdBuffer),
                                                            {0});
        }

        // The static survivors through the static id pipeline (same plan/draw shape as the
        // g-buffer pass; only the bound pipeline and attachments differ — id, not g-buffer).
        if (!plan.Slots.empty() && staticPipeline)
        {
            cmd.BindPipeline(staticPipeline);
            registry.Bind(cmd);
            cmd.BindDescriptorSets(DescriptorSetBindInfo{
                .Sets = {plan.DrawDataSet},
                .FirstSet = 3,
                .PipelineBindPoint = PipelineBindPoint::Graphics,
            });
            cmd.PushConstants(plan.Push);

            const Mesh* lastBound = nullptr;
            for (const DrawGroup& group : plan.Groups)
            {
                if (lastBound != group.SourceMesh)
                {
                    cmd.BindVertexBuffer(group.SourceMesh->GetVertexBuffer());
                    cmd.BindIndexBuffer(group.SourceMesh->GetIndexBuffer());
                    lastBound = group.SourceMesh;
                }
                if (plan.Cull == SceneRendererSettings::CullMode::GPU)
                {
                    const u64 offset =
                        plan.IndirectRegionOffset +
                        static_cast<u64>(group.FirstSlot) * sizeof(DrawIndexedIndirectCommand);
                    cmd.DrawIndexedIndirect(plan.IndirectBuffer, offset, group.SlotCount,
                                            sizeof(DrawIndexedIndirectCommand));
                }
                else
                {
                    for (u32 s = 0; s < group.SlotCount; ++s)
                    {
                        const DrawSlot& slot = plan.Slots[group.FirstSlot + s];
                        cmd.DrawIndexed(slot.IndexCount, 1, slot.FirstIndex, slot.VertexOffset,
                                        slot.CandidateId);
                    }
                }
            }
        }

        // The skinned survivors through the skinned id pipeline + the palette set (set 2).
        if (!plan.SkinnedSlots.empty() && skinnedPipeline)
        {
            cmd.BindPipeline(skinnedPipeline);
            registry.Bind(cmd);
            cmd.BindDescriptorSets(DescriptorSetBindInfo{
                .Sets = {plan.DrawDataSet},
                .FirstSet = 3,
                .PipelineBindPoint = PipelineBindPoint::Graphics,
            });
            cmd.BindDescriptorSets(DescriptorSetBindInfo{
                .Sets = {plan.PaletteSet},
                .FirstSet = 4,
                .PipelineBindPoint = PipelineBindPoint::Graphics,
            });
            cmd.PushConstants(plan.Push);

            const Mesh* lastBound = nullptr;
            for (const DrawGroup& group : plan.SkinnedGroups)
            {
                if (lastBound != group.SourceMesh)
                {
                    cmd.BindVertexBuffer(group.SourceMesh->GetVertexBuffer());
                    cmd.BindIndexBuffer(group.SourceMesh->GetIndexBuffer());
                    lastBound = group.SourceMesh;
                }
                for (u32 s = 0; s < group.SlotCount; ++s)
                {
                    const DrawSlot& slot = plan.SkinnedSlots[group.FirstSlot + s];
                    cmd.DrawIndexed(slot.IndexCount, 1, slot.FirstIndex, slot.VertexOffset,
                                    slot.CandidateId);
                }
            }
        }
    }
}
