#include "SceneLayersScenePass.h"

#include <algorithm>

namespace Veng::Renderer
{
    SceneLayersScenePass::SceneLayersScenePass(const SceneLayerTargets& targets,
                                               vector<Unique<SceneLayer>> layers)
        : m_Targets(targets), m_Layers(std::move(layers))
    {
    }

    void SceneLayersScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        for (const Unique<SceneLayer>& layer : m_Layers)
        {
            layer->DeclareAhead(graph, io);
        }

        RenderGraph::PassBuilder builder = graph.AddPass("Scene Layers");
        builder.Color(
            {.Resource = m_Targets.Target, .Load = LoadOp::Load, .Store = StoreOp::Store});
        if (m_Targets.Mask.IsValid())
        {
            // The first translucent pass cleared the mask; the layers add to it.
            builder.Color(
                {.Resource = m_Targets.Mask, .Load = LoadOp::Load, .Store = StoreOp::Store});
        }
        if (m_Targets.Reactive.IsValid())
        {
            // The first translucent pass cleared it too; the layers add their coverage.
            builder.Color(
                {.Resource = m_Targets.Reactive, .Load = LoadOp::Load, .Store = StoreOp::Store});
        }
        builder.Depth({.Resource = m_Targets.Depth, .Load = LoadOp::Load, .Store = StoreOp::Store});
        for (const Unique<SceneLayer>& layer : m_Layers)
        {
            layer->DeclareInputs(builder, io);
        }
        // A frame no layer draws in (deactivation hysteresis) skips rather than loading and storing
        // its targets around no draws.
        builder.SkipWhen(
            [this]
            {
                return std::ranges::none_of(m_Layers, [](const Unique<SceneLayer>& layer)
                                            { return layer->IsDrawing(); });
            });
        builder.Execute(
            [this](PassContext& inner)
            {
                const ScenePassContext ctx = Wrap(inner);
                for (const Unique<SceneLayer>& layer : m_Layers)
                {
                    if (layer->IsDrawing())
                    {
                        layer->Record(ctx);
                    }
                }
            });
    }
}
