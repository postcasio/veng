#include "SurfaceShadowPipelines.h"

#include <functional>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/PipelineLayout.h>

namespace Veng::Renderer
{
    Unique<SurfaceShadowPipelines>
    SurfaceShadowPipelines::Create(Context& context, AssetManager& assets,
                                   Ref<DescriptorSetLayout> shadowLayout)
    {
        return Unique<SurfaceShadowPipelines>(
            new SurfaceShadowPipelines(context, assets, std::move(shadowLayout)));
    }

    SurfaceShadowPipelines::SurfaceShadowPipelines(Context& context, AssetManager& assets,
                                                   Ref<DescriptorSetLayout> shadowLayout)
        : m_Context(context), m_Assets(assets), m_ShadowLayout(std::move(shadowLayout))
    {
    }

    SurfaceShadowPipelines::~SurfaceShadowPipelines() = default;

    usize SurfaceShadowPipelines::KeyHash::operator()(const Key& key) const
    {
        return std::hash<const PipelineLayout*>{}(key.Layout) ^
               (static_cast<usize>(key.Kind) * 0x9E3779B97F4A7C15ULL);
    }

    void SurfaceShadowPipelines::SetShadowSet(Ref<DescriptorSet> set, const u32 shadowRingStride,
                                              const u32 punctualRingStride)
    {
        m_ShadowSet = std::move(set);
        m_ShadowRingStride = shadowRingStride;
        m_PunctualRingStride = punctualRingStride;
    }

    bool SurfaceShadowPipelines::BindPipeline(CommandBuffer& cmd, const MaterialInstance& material,
                                              const SurfacePipelineKind kind)
    {
        const Material& parent = *material.GetParent().Get();
        if (!parent.ReadsShadowMaps())
        {
            switch (kind)
            {
            case SurfacePipelineKind::Static:
                material.Bind(cmd);
                break;
            case SurfacePipelineKind::Skinned:
                material.BindSkinned(cmd);
                break;
            case SurfacePipelineKind::Instanced:
                material.BindInstanced(cmd);
                break;
            }
            return false;
        }

        const Key key{.Layout = parent.GetPipelineLayout().get(), .Kind = kind};
        auto it = m_Pipelines.find(key);
        if (it == m_Pipelines.end())
        {
            Result<Ref<GraphicsPipeline>> built =
                parent.BuildShadowReadingPipeline(m_Assets, kind, m_ShadowLayout);
            VE_ASSERT(built.has_value(),
                      "SurfaceShadowPipelines: the shadow-reading pipeline for '{}' failed to "
                      "build: {}",
                      parent.GetName(), built.has_value() ? "" : built.error());
            it = m_Pipelines
                     .emplace(key, Entry{.MaterialLayout = parent.GetPipelineLayout(),
                                         .Pipeline = std::move(*built)})
                     .first;
        }
        cmd.BindPipeline(it->second.Pipeline);
        return true;
    }

    void SurfaceShadowPipelines::BindShadowSet(CommandBuffer& cmd) const
    {
        VE_ASSERT(m_ShadowSet != nullptr, "SurfaceShadowPipelines: no shadow set was given");
        const u32 frameSlot = m_Context.GetCurrentFrameInFlight();
        cmd.BindDescriptorSets({m_ShadowSet.get()}, SurfaceShadowMapSet,
                               PipelineBindPoint::Graphics,
                               {frameSlot * m_ShadowRingStride, frameSlot * m_PunctualRingStride});
    }
}
