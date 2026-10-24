#include "SceneColorDownsampleTailScenePass.h"

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/DescriptorSet.h>

namespace Veng::Renderer
{
    SceneColorDownsampleTailScenePass::SceneColorDownsampleTailScenePass(
        string name, Ref<ComputePipeline> pipeline, Ref<DescriptorSet> set,
        const ResourceId sourceId, const std::span<const ResourceId> levelIds, const u32 firstLevel,
        const uvec2 extent, const bool* sampled, const SceneColorChainRegion region)
        : m_Pipeline(std::move(pipeline)), m_Set(std::move(set)), m_SourceId(sourceId),
          m_LevelIds(levelIds.begin(), levelIds.end()), m_FirstLevel(firstLevel), m_Extent(extent),
          m_Sampled(sampled), m_Region(region), m_Name(std::move(name))
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
        builder.SkipWhenUnread([sampled = m_Sampled] { return !*sampled; });
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
                    .RenderExtent =
                        SceneColorChainValidExtent(m_Region, m_Extent, Wrap(inner).View()),
                    .AllocBase = m_Extent,
                    .FirstLevel = m_FirstLevel,
                    .LevelCount = static_cast<u32>(m_LevelIds.size()),
                });
                cmd.Dispatch(1, 1, 1);
            });
    }
}
