#include "GBufferShadingOverride.h"

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/PipelineLayout.h>

#include "DrawPlan.h"

namespace Veng::Renderer
{
    namespace
    {
        /// @brief The core pack's override fragment (gbuffer_shading_override.frag).
        constexpr AssetId ShadingOverrideFragId{0x7900FF4E551FA995ULL};

        /// @brief The layout a material's static or skinned g-buffer pipeline is bound against.
        [[nodiscard]] const PipelineLayout* LayoutOf(const MaterialInstance& material,
                                                     const bool skinned)
        {
            return skinned ? material.GetSkinnedPipelineLayout().get()
                           : material.GetPipelineLayout().get();
        }
    }

    Unique<GBufferShadingOverride> GBufferShadingOverride::Create(AssetManager& assets)
    {
        return Unique<GBufferShadingOverride>(new GBufferShadingOverride(assets));
    }

    GBufferShadingOverride::GBufferShadingOverride(AssetManager& assets) : m_Assets(assets) {}

    GBufferShadingOverride::~GBufferShadingOverride() = default;

    void GBufferShadingOverride::Prepare(const GBufferDrawPlan& plan)
    {
        if (!m_Fragment.IsLoaded())
        {
            AssetResult<AssetHandle<Shader>> fragment =
                m_Assets.LoadSync<Shader>(ShadingOverrideFragId);
            VE_ASSERT(fragment.has_value(),
                      "GBufferShadingOverride: the override fragment failed to load: {}",
                      fragment.has_value() ? "" : fragment.error().Detail);
            m_Fragment = std::move(*fragment);
        }

        // A group's material changes only where its pipeline did, so most groups find theirs on the
        // first lookup.
        for (const DrawGroup& group : plan.Groups)
        {
            Ensure(*group.PipelineMaterial, false);
        }
        for (const DrawGroup& group : plan.SkinnedGroups)
        {
            Ensure(*group.PipelineMaterial, true);
        }
    }

    void GBufferShadingOverride::Ensure(const MaterialInstance& material, const bool skinned)
    {
        const PipelineLayout* const key = LayoutOf(material, skinned);
        if (m_Pipelines.contains(key))
        {
            return;
        }

        const Material& parent = *material.GetParent().Get();
        Result<Ref<GraphicsPipeline>> built =
            parent.BuildFragmentOverridePipeline(m_Assets, *m_Fragment.Get(), skinned);
        VE_ASSERT(built.has_value(),
                  "GBufferShadingOverride: the override pipeline for '{}' failed to build: {}",
                  parent.GetName(), built.has_value() ? "" : built.error());
        m_Pipelines.emplace(key, Entry{.Layout = skinned ? material.GetSkinnedPipelineLayout()
                                                         : material.GetPipelineLayout(),
                                       .Pipeline = std::move(*built)});
    }

    const Ref<GraphicsPipeline>& GBufferShadingOverride::Get(const MaterialInstance& material,
                                                             const bool skinned) const
    {
        const auto it = m_Pipelines.find(LayoutOf(material, skinned));
        VE_ASSERT(it != m_Pipelines.end(),
                  "GBufferShadingOverride: no override pipeline was prepared for '{}'",
                  material.GetName());
        return it->second.Pipeline;
    }

    void GBufferShadingOverride::Release()
    {
        m_Pipelines.clear();
        m_Fragment = {};
    }
}
