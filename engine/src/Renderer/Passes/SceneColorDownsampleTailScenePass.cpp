#include "SceneColorDownsampleTailScenePass.h"

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/DescriptorSet.h>

#include <fmt/format.h>

namespace Veng::Renderer
{
    SceneColorDownsampleTailScenePass::SceneColorDownsampleTailScenePass(
        Ref<ComputePipeline> pipeline, Ref<DescriptorSet> set, const ResourceId sourceId,
        const std::span<const ResourceId> levelIds, const u32 firstLevel, const uvec2 extent)
        : m_Pipeline(std::move(pipeline)), m_Set(std::move(set)), m_SourceId(sourceId),
          m_LevelIds(levelIds.begin(), levelIds.end()), m_FirstLevel(firstLevel), m_Extent(extent),
          m_Name(fmt::format("Scene Color Downsample Tail {}-{}", firstLevel,
                             firstLevel + static_cast<u32>(levelIds.size()) - 1))
    {
    }

    void SceneColorDownsampleTailScenePass::Declare(RenderGraph& graph, const PassIO& /*io*/)
    {
        RenderGraph::PassBuilder builder = graph.AddComputePass(m_Name);
        builder.Sample(m_SourceId);
        for (const ResourceId level : m_LevelIds)
        {
            builder.StorageWrite(level);
        }
        builder.Execute(
            [this](PassContext& inner)
            {
                CommandBuffer& cmd = inner.Cmd();
                cmd.BindPipeline(m_Pipeline);
                cmd.BindDescriptorSets(DescriptorSetBindInfo{
                    .Sets = {m_Set},
                    .FirstSet = 3,
                    .PipelineBindPoint = PipelineBindPoint::Compute,
                });
                cmd.PushConstants(SceneColorDownsampleTailPush{
                    .RenderExtent = Wrap(inner).View().RenderExtent,
                    .AllocBase = m_Extent,
                    .FirstLevel = m_FirstLevel,
                    .LevelCount = static_cast<u32>(m_LevelIds.size()),
                });
                cmd.Dispatch(1, 1, 1);
            });
    }
}
