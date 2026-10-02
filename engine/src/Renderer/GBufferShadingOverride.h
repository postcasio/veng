#pragma once

#include <unordered_map>

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Veng.h>

namespace Veng
{
    class AssetManager;
    class MaterialInstance;
    struct Shader;
}

namespace Veng::Renderer
{
    class GraphicsPipeline;
    class PipelineLayout;
    struct GBufferDrawPlan;

    /// @brief Owns the pipelines the g-buffer pass draws through under
    ///        SceneRendererSettings::GBufferShadingOverride.
    ///
    /// Whether a deferred renderer's g-buffer pass is bound by its materials' shading or by its
    /// geometry decides what could make it cheaper, and the pass's time alone cannot say which. The
    /// override answers it by replacing only the fragment stage: for every material the pass draws,
    /// it builds a pipeline from that material's own vertex stage, pipeline layout and face culling
    /// with a trivial fragment (gbuffer_shading_override.frag), so the pass records the same binds
    /// and the same draws over the same geometry and only the shading changes.
    ///
    /// Pipelines are keyed on the layout they were built against — a material's static and skinned
    /// layouts are its own, so the key names one pipeline of one material — and each entry holds that
    /// layout, so the key cannot be reused by another material while the entry lives. Entries are
    /// built the first frame their material is drawn with the override on and dropped together when
    /// it is turned off; a dropped pipeline retires deferred, past the frames still reading it.
    class GBufferShadingOverride
    {
    public:
        /// @brief Creates the override with nothing built; the fragment shader loads on first use.
        /// @param assets Asset manager the fragment shader and the vertex stages' layouts load from.
        /// @return A new GBufferShadingOverride.
        static Unique<GBufferShadingOverride> Create(AssetManager& assets);

        ~GBufferShadingOverride();

        GBufferShadingOverride(const GBufferShadingOverride&) = delete;
        GBufferShadingOverride& operator=(const GBufferShadingOverride&) = delete;

        /// @brief Builds the override pipeline of every material the plan draws that has none yet.
        ///
        /// Runs before the graph replays, after the skinned materials' own skinned pipelines are
        /// built (a skinned override reuses that layout). Fatal if a build fails: a Surface
        /// material's vertex stage writes the surface interpolants the override reads.
        /// @param plan The frame's g-buffer draw plan.
        void Prepare(const GBufferDrawPlan& plan);

        /// @brief Returns the override pipeline for a material's static or skinned draws.
        /// @pre Prepare ran over a plan drawing @p material that way this frame.
        /// @param material The draw group's material.
        /// @param skinned  Whether the group is a skinned one.
        /// @return The pipeline to bind in place of the material's own.
        [[nodiscard]] const Ref<GraphicsPipeline>& Get(const MaterialInstance& material,
                                                       bool skinned) const;

        /// @brief Drops every built pipeline and the fragment shader (the override was turned off).
        void Release();

        /// @brief The number of pipelines built and held.
        [[nodiscard]] usize GetPipelineCount() const { return m_Pipelines.size(); }

    private:
        explicit GBufferShadingOverride(AssetManager& assets);

        /// @brief Builds the pipeline for one material and layout if it is not held yet.
        void Ensure(const MaterialInstance& material, bool skinned);

        /// @brief One built pipeline and the layout its key names.
        struct Entry
        {
            /// @brief Held so the key's address cannot name another layout while this lives.
            Ref<PipelineLayout> Layout;
            /// @brief The material's geometry state with the override fragment.
            Ref<GraphicsPipeline> Pipeline;
        };

        /// @brief Asset manager the shaders and vertex layouts load from.
        AssetManager& m_Assets;
        /// @brief The override fragment shader, loaded on first Prepare.
        AssetHandle<Shader> m_Fragment;
        /// @brief Built pipelines, keyed on the material layout each was built against.
        std::unordered_map<const PipelineLayout*, Entry> m_Pipelines;
    };
}
