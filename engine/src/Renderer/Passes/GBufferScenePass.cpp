#include "GBufferScenePass.h"

#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/Native.h>

namespace Veng::Renderer
{
    void GBufferScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        RenderGraph::PassBuilder builder = graph.AddPass("Scene GBuffer");
        builder
            .Color({
                .Resource = io.GBufferAlbedo,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.05f, .G = 0.05f, .B = 0.08f, .A = 1.0f},
            })
            .Color({
                .Resource = io.GBufferNormal,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
            })
            .Color({
                .Resource = io.GBufferOrm,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                // Default occlusion 1 (unoccluded), roughness/metallic/emissive 0
                // for any background texel; a material overwrites all four.
                .Clear = ClearColor{.R = 1.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
            })
            .Color({
                // G3 — the per-object motion vector (SV_Target3). The surface fragment
                // writes it alongside the g-buffer, so motion vectors cost no second
                // geometry pass. Cleared to zero motion for any background texel; the
                // TAA resolve falls back to depth reprojection wherever it stays zero.
                .Resource = io.Velocity,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
            })
            .Color({
                // G4 — HDR emissive (SV_Target4). The surface fragment writes authored
                // emission alongside the g-buffer; the lighting pass adds it into the
                // pixel's outgoing light. Cleared to zero so an un-drawn pixel emits nothing.
                .Resource = io.GBufferEmissive,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
            })
            .Depth({
                .Resource = io.GBufferDepth,
                .Load = LoadOp::Clear,
                // Stored: the lighting pass reads depth as a texture.
                .Store = StoreOp::Store,
                .Clear = ClearDepth{.Depth = 1.0f, .Stencil = 0},
            });

        // GPU mode reads the cull-written commands as indirect args; declaring the
        // read drives the graph-derived StorageBufferWrite → IndirectRead barrier.
        if (m_Cull == SceneRendererSettings::CullMode::GPU)
        {
            builder.IndirectRead(m_IndirectId);
        }

        builder.Execute([this](PassContext& inner) { Record(Wrap(inner)); });
    }

    void GBufferScenePass::Record(const ScenePassContext& ctx) const
    {
        CommandBuffer& cmd = ctx.Cmd();
        const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
        const GBufferDrawPlan& plan = *m_Plan;

        // Render into the dynamic-resolution sub-rect; the target stays allocated at the
        // high-water-mark extent (m_Extent), so the consumer upscales the valid region.
        const uvec2 renderExtent = ctx.View().RenderExtent;
        cmd.SetViewport({0, 0}, renderExtent);
        cmd.SetScissor({0, 0}, renderExtent);

        if (plan.Slots.empty() && plan.SkinnedSlots.empty())
        {
            return;
        }

        // The instance-rate candidate-id buffer (binding 1) is bound once for both the
        // static and skinned passes; each draw's firstInstance selects the candidate id,
        // fetched as the instance attribute that indexes DrawData.
        if (!plan.Slots.empty() || !plan.SkinnedSlots.empty())
        {
            cmd.GetNative().CommandBuffer.bindVertexBuffers(1, GetVkBuffer(*plan.CandidateIdBuffer),
                                                            {0});
        }

        if (!plan.Slots.empty())
        {
            // The fragment pipeline is not shared across surface materials — a custom
            // fragment shader is its own pipeline — so it binds per group, keyed on the
            // group's material. Set 0 (bindless), set 1 (the per-draw DrawData SSBO), and
            // the frame selector push share the surface pipeline layout (core surface.vert
            // + the deferred g-buffer formats), so they (re)bind against whichever pipeline
            // is current; binding them right after each pipeline bind keeps a valid layout.
            const Mesh* lastBound = nullptr;
            const MaterialInstance* lastPipeline = nullptr;
            for (const DrawGroup& group : plan.Groups)
            {
                if (lastPipeline != group.PipelineMaterial)
                {
                    group.PipelineMaterial->Bind(cmd);
                    registry.Bind(cmd);
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {plan.DrawDataSet},
                        .FirstSet = 1,
                        .PipelineBindPoint = PipelineBindPoint::Graphics,
                    });
                    cmd.PushConstants(plan.Push);
                    lastPipeline = group.PipelineMaterial;
                }
                if (lastBound != group.SourceMesh)
                {
                    cmd.BindVertexBuffer(group.SourceMesh->GetVertexBuffer());
                    cmd.BindIndexBuffer(group.SourceMesh->GetIndexBuffer());
                    lastBound = group.SourceMesh;
                }

                if (plan.Cull == SceneRendererSettings::CullMode::GPU)
                {
                    // One indirect draw over the group's contiguous command run; culled
                    // slots carry instanceCount 0 and no-op (no GPU-sourced count —
                    // MoltenVK lacks drawIndirectCount).
                    const u64 offset =
                        plan.IndirectRegionOffset +
                        static_cast<u64>(group.FirstSlot) * sizeof(DrawIndexedIndirectCommand);
                    cmd.DrawIndexedIndirect(plan.IndirectBuffer, offset, group.SlotCount,
                                            sizeof(DrawIndexedIndirectCommand));
                }
                else
                {
                    // CPU mode issues a direct DrawIndexed per surviving slot, the
                    // candidate id carried as firstInstance (the same instance path).
                    for (u32 s = 0; s < group.SlotCount; ++s)
                    {
                        const DrawSlot& slot = plan.Slots[group.FirstSlot + s];
                        cmd.DrawIndexed(slot.IndexCount, 1, slot.FirstIndex, slot.VertexOffset,
                                        slot.CandidateId);
                    }
                }
            }
        }

        // Skinned draws: the skinned surface pipeline + the palette set (set 2), always
        // CPU-direct (skinned meshes opt out of GPU-driven culling). They share the same
        // DrawData buffer; each slot's DrawData.PaletteBase points into the palette. As on
        // the static path the fragment pipeline binds per group, keyed on the group's
        // material; set 0 / set 1 / set 2 / the push (re)bind against its shared layout.
        if (!plan.SkinnedSlots.empty())
        {
            const Mesh* lastBound = nullptr;
            const MaterialInstance* lastPipeline = nullptr;
            for (const DrawGroup& group : plan.SkinnedGroups)
            {
                if (lastPipeline != group.PipelineMaterial)
                {
                    // Bind the material's skinned g-buffer pipeline, not its static one: the skinned
                    // pipeline's layout carries the palette at set 2, so the bind below is valid.
                    group.PipelineMaterial->BindSkinned(cmd);
                    registry.Bind(cmd);
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {plan.DrawDataSet},
                        .FirstSet = 1,
                        .PipelineBindPoint = PipelineBindPoint::Graphics,
                    });
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {plan.PaletteSet},
                        .FirstSet = 2,
                        .PipelineBindPoint = PipelineBindPoint::Graphics,
                    });
                    cmd.PushConstants(plan.Push);
                    lastPipeline = group.PipelineMaterial;
                }
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
