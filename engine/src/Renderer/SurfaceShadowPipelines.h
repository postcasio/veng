#pragma once

#include <unordered_map>

#include <Veng/Asset/Material.h>
#include <Veng/Veng.h>

namespace Veng
{
    class AssetManager;
    class MaterialInstance;
}

namespace Veng::Renderer
{
    class CommandBuffer;
    class Context;
    class DescriptorSet;
    class DescriptorSetLayout;
    class GraphicsPipeline;
    class PipelineLayout;

    /// @brief Binds a Surface material's g-buffer pipeline, and the renderer's shadow set for one
    ///        that reads the shadow maps.
    ///
    /// A material whose fragment reads the shadow maps (Material::ReadsShadowMaps) holds no
    /// pipeline of its own, since the shadow set's layout carries the renderer's immutable
    /// comparison sampler and only a pipeline built against that layout object's definition
    /// accepts the set. This builds each such material's pipelines — one per SurfacePipelineKind —
    /// against the renderer's shadow layout the first time it is drawn that way, and binds them in
    /// place of the material's own; every other material binds exactly as it would on its own.
    ///
    /// Pipelines are keyed on the material's own pipeline layout and the kind, and each entry holds
    /// that layout, so the key cannot name another material while the entry lives (the
    /// GBufferShadingOverride shape). The shadow layout outlives every rebuild of the shadow set, so
    /// the pipelines stay valid across them; the set itself is replaced by SetShadowSet.
    class SurfaceShadowPipelines
    {
    public:
        /// @brief Creates the cache with nothing built.
        /// @param context      Render context, for the frame-in-flight the set's offsets select.
        /// @param assets       Asset manager the vertex stages and their layouts load from.
        /// @param shadowLayout The renderer's shadow set layout, held for the cache's lifetime.
        /// @return A new SurfaceShadowPipelines.
        static Unique<SurfaceShadowPipelines> Create(Context& context, AssetManager& assets,
                                                     Ref<DescriptorSetLayout> shadowLayout);

        ~SurfaceShadowPipelines();

        SurfaceShadowPipelines(const SurfaceShadowPipelines&) = delete;
        SurfaceShadowPipelines& operator=(const SurfaceShadowPipelines&) = delete;

        /// @brief Points the binds at the renderer's current shadow set and its ring strides.
        /// @param set              The shadow set (rebuilt with the shadow topology).
        /// @param shadowRingStride The ShadowConstants ring stride (the first dynamic offset).
        /// @param punctualRingStride The PunctualShadows ring stride (the second dynamic offset).
        void SetShadowSet(Ref<DescriptorSet> set, u32 shadowRingStride, u32 punctualRingStride);

        /// @brief Binds @p material's g-buffer pipeline of @p kind.
        ///
        /// A material reading no shadow map binds its own (Bind, BindSkinned or BindInstanced); a
        /// shadow-reading one binds the pipeline built here, building it on first use. Fatal if the
        /// build fails.
        /// @param cmd      The command buffer to record into.
        /// @param material The draw's material.
        /// @param kind     The vertex stage the draw goes through.
        /// @return Whether the material reads the shadow maps, so the caller binds the shadow set
        ///         (BindShadowSet) after its own sets.
        bool BindPipeline(CommandBuffer& cmd, const MaterialInstance& material,
                          SurfacePipelineKind kind);

        /// @brief Binds the shadow set at SurfaceShadowMapSet with this frame's ring offsets.
        /// @pre A pipeline BindPipeline reported shadow-reading is bound.
        /// @param cmd The command buffer to record into.
        void BindShadowSet(CommandBuffer& cmd) const;

        /// @brief The number of pipelines built and held.
        [[nodiscard]] usize GetPipelineCount() const { return m_Pipelines.size(); }

    private:
        SurfaceShadowPipelines(Context& context, AssetManager& assets,
                               Ref<DescriptorSetLayout> shadowLayout);

        /// @brief One built pipeline and the material layout its key names.
        struct Entry
        {
            /// @brief Held so the key's address cannot name another material while this lives.
            Ref<PipelineLayout> MaterialLayout;
            /// @brief The pipeline built against the shadow layout.
            Ref<GraphicsPipeline> Pipeline;
        };

        /// @brief A material's own layout and the kind of pipeline built for it.
        struct Key
        {
            /// @brief The material's own pipeline layout.
            const PipelineLayout* Layout = nullptr;
            /// @brief The kind of pipeline.
            SurfacePipelineKind Kind = SurfacePipelineKind::Static;

            /// @brief Equality over both members.
            bool operator==(const Key&) const = default;
        };

        /// @brief Hashes a Key.
        struct KeyHash
        {
            /// @brief The key's hash.
            usize operator()(const Key& key) const;
        };

        /// @brief Render context, for the current frame-in-flight.
        Context& m_Context;
        /// @brief Asset manager the vertex stages load from.
        AssetManager& m_Assets;
        /// @brief The renderer's shadow set layout.
        Ref<DescriptorSetLayout> m_ShadowLayout;
        /// @brief The renderer's current shadow set.
        Ref<DescriptorSet> m_ShadowSet;
        /// @brief The ShadowConstants ring stride.
        u32 m_ShadowRingStride = 0;
        /// @brief The PunctualShadows ring stride.
        u32 m_PunctualRingStride = 0;
        /// @brief Built pipelines.
        std::unordered_map<Key, Entry, KeyHash> m_Pipelines;
    };
}
