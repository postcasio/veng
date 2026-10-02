#include "GBufferScenePass.h"

#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>

namespace Veng::Renderer
{
    namespace
    {
        [[nodiscard]] StoreOp StoreOf(const GBufferChannelState state)
        {
            return state == GBufferChannelState::Stored ? StoreOp::Store : StoreOp::DontCare;
        }
    }

    void RecordInstanceRuns(CommandBuffer& cmd, const std::span<const DrawSlot> slots,
                            const std::span<const InstanceRun> runs, const DrawGroup& group)
    {
        for (u32 r = 0; r < group.RunCount; ++r)
        {
            const InstanceRun& run = runs[group.FirstRun + r];
            const DrawSlot& first = slots[run.FirstSlot];
            cmd.DrawIndexed(first.IndexCount, run.Count, first.FirstIndex, first.VertexOffset,
                            first.CandidateId);
        }
    }

    void GBufferScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        RenderGraph::PassBuilder builder = graph.AddPass("Scene GBuffer");
        builder
            .Color({
                .Resource = io.GBufferAlbedo,
                .Load = LoadOp::Clear,
                .Store = StoreOf(m_Stores.Albedo),
                .Clear = ClearColor{.R = 0.05f, .G = 0.05f, .B = 0.08f, .A = 1.0f},
            })
            .Color({
                .Resource = io.GBufferNormal,
                .Load = LoadOp::Clear,
                .Store = StoreOf(m_Stores.Normal),
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
            })
            .Color({
                .Resource = io.GBufferOrm,
                .Load = LoadOp::Clear,
                .Store = StoreOf(m_Stores.Orm),
                // Default occlusion 1 (unoccluded), roughness/metallic/emissive 0
                // for any background texel; a material overwrites all four.
                .Clear = ClearColor{.R = 1.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
            })
            .Color({
                // G3 — the per-object motion vector (SV_Target3). The surface fragment
                // writes it alongside the g-buffer, so motion vectors cost no second
                // geometry pass. Cleared to zero motion for any background texel; the
                // TAA resolve falls back to depth reprojection wherever it stays zero.
                // Discarded on a frame with no reader (see FrameTopology::GBufferStores).
                .Resource = io.Velocity,
                .Load = LoadOp::Clear,
                .Store = StoreOf(m_Stores.Velocity),
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
            })
            .Color({
                // G4 — HDR emissive (SV_Target4). The surface fragment writes authored
                // emission alongside the g-buffer; the lighting pass adds it into the
                // pixel's outgoing light. Cleared to zero so an un-drawn pixel emits nothing.
                .Resource = io.GBufferEmissive,
                .Load = LoadOp::Clear,
                .Store = StoreOf(m_Stores.Emissive),
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
            })
            .Depth({
                .Resource = io.GBufferDepth,
                .Load = LoadOp::Clear,
                // Stored: the lighting pass reads depth as a texture.
                .Store = StoreOp::Store,
                // Reverse-Z: the far plane is 0, so an un-drawn pixel clears to 0.
                .Clear = ClearDepth{.Depth = 0.0f, .Stencil = 0},
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
            cmd.BindInstanceBuffer(plan.CandidateIdBuffer);
        }

        if (!plan.Slots.empty())
        {
            // The fragment pipeline is not shared across surface materials — a custom
            // fragment shader is its own pipeline — so it binds per group, keyed on the
            // group's parent material. The slots were ordered by (parent, mesh, submesh), so
            // each pipeline binds once and each mesh's buffers bind once per pipeline. Set 0
            // (bindless), set 3 (the per-draw DrawData SSBO), and the frame selector push share
            // the surface pipeline layout (core surface.vert + the deferred g-buffer formats),
            // so they (re)bind against whichever pipeline is current; binding them right after
            // each pipeline bind keeps a valid layout.
            const Mesh* lastBound = nullptr;
            const MaterialInstance* lastPipeline = nullptr;
            for (const DrawGroup& group : plan.Groups)
            {
                if (lastPipeline != group.PipelineMaterial)
                {
                    group.PipelineMaterial->Bind(cmd);
                    registry.Bind(cmd);
                    cmd.BindDescriptorSets({plan.DrawDataSet.get()}, 3);
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
                    // CPU mode issues one instanced DrawIndexed per run of equal slots, the first
                    // slot's candidate id carried as firstInstance: instance i reads candidate
                    // firstInstance + i through the identity candidate-id buffer.
                    RecordInstanceRuns(cmd, plan.Slots, plan.Runs, group);
                }
            }
        }

        // Skinned draws: the skinned surface pipeline + the palette set (set 4), always
        // CPU-direct (skinned meshes opt out of GPU-driven culling). They share the same
        // DrawData buffer; each slot's DrawData.PaletteBase points into the palette. As on
        // the static path the fragment pipeline binds per group, keyed on the group's
        // material; set 0 / set 3 / set 4 / the push (re)bind against its shared layout.
        if (!plan.SkinnedSlots.empty())
        {
            const Mesh* lastBound = nullptr;
            const MaterialInstance* lastPipeline = nullptr;
            for (const DrawGroup& group : plan.SkinnedGroups)
            {
                if (lastPipeline != group.PipelineMaterial)
                {
                    // Bind the material's skinned g-buffer pipeline, not its static one: the skinned
                    // pipeline's layout carries the palette at set 4, so the bind below is valid.
                    group.PipelineMaterial->BindSkinned(cmd);
                    registry.Bind(cmd);
                    cmd.BindDescriptorSets({plan.DrawDataSet.get(), plan.PaletteSet.get()}, 3);
                    cmd.PushConstants(plan.Push);
                    lastPipeline = group.PipelineMaterial;
                }
                if (lastBound != group.SourceMesh)
                {
                    cmd.BindVertexBuffer(group.SourceMesh->GetVertexBuffer());
                    cmd.BindIndexBuffer(group.SourceMesh->GetIndexBuffer());
                    lastBound = group.SourceMesh;
                }
                RecordInstanceRuns(cmd, plan.SkinnedSlots, plan.SkinnedRuns, group);
            }
        }
    }
}
