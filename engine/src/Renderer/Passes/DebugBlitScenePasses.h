#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    class Context;
    class DescriptorSet;
    class GraphicsPipeline;

    // Push block shared by all single-target debug blits: a texture + sampler index.
    struct BlitPushConstants
    {
        u32 Texture;
        u32 Sampler;
    };

    // ORM-channel blit push block: ORM texture + sampler + channel selector
    // (0 occlusion / 1 roughness / 2 metallic). Shared by all three ORM debug arms.
    struct OrmBlitPushConstants
    {
        u32 Texture;
        u32 Sampler;
        u32 Channel;
    };

    /// @brief Fullscreen debug blit of a single bindless-sampled target into the output.
    ///
    /// The declared .Sample on the source id drives the graph-derived attachment → shader-read
    /// transition. The shadow atlases are off bindless and handled by ShadowBlitScenePass.
    class FullscreenBlitScenePass final : public ScenePass
    {
    public:
        /// @brief Which PassIO target this blit reads from.
        ///
        /// The shadow atlases are off bindless and handled by ShadowBlitScenePass, not this class.
        enum class Source
        {
            Albedo,
            Normal,
            Depth,
            Ao,
            Bloom,
            MotionVectors,
            Reflections,
            Emissive,
            Coc
        };

        /// @brief Constructs the pass.
        /// @param context  Renderer context for bindless access.
        /// @param pipeline The blit pipeline (the fragment shader selects the visualization).
        /// @param extent   Initial render extent; updated via Resize.
        /// @param source   Which g-buffer/battery target this pass samples.
        FullscreenBlitScenePass(Context& context, Ref<GraphicsPipeline> pipeline, uvec2 extent,
                                Source source)
            : m_Context(context), m_Pipeline(std::move(pipeline)), m_Extent(extent),
              m_Source(source)
        {
        }

        /// @brief Updates the render extent.
        void Resize(uvec2 extent) override { m_Extent = extent; }
        /// @brief Contributes the debug blit pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Resolves the source's imported resource id from PassIO.
        [[nodiscard]] ResourceId SourceId(const PassIO& io) const;
        /// @brief Resolves the source's bindless texture slot from PassIO.
        [[nodiscard]] TextureHandle SourceHandle(const PassIO& io) const;

        /// @brief Renderer context for bindless access.
        Context& m_Context;
        /// @brief The blit pipeline.
        Ref<GraphicsPipeline> m_Pipeline;
        /// @brief Current render extent.
        uvec2 m_Extent;
        /// @brief Which target this pass samples.
        Source m_Source;
    };

    /// @brief Fullscreen debug blit of one packed-ORM channel into the output.
    ///
    /// Channel: 0 = occlusion, 1 = roughness, 2 = metallic (matches OrmBlitPushConstants::Channel).
    class OrmBlitScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass.
        /// @param context  Renderer context for bindless access.
        /// @param pipeline The ORM blit pipeline.
        /// @param extent   Initial render extent; updated via Resize.
        /// @param channel  The ORM channel selector (0 occlusion / 1 roughness / 2 metallic).
        OrmBlitScenePass(Context& context, Ref<GraphicsPipeline> pipeline, uvec2 extent,
                         u32 channel)
            : m_Context(context), m_Pipeline(std::move(pipeline)), m_Extent(extent),
              m_Channel(channel)
        {
        }

        /// @brief Updates the render extent.
        void Resize(uvec2 extent) override { m_Extent = extent; }
        /// @brief Contributes the ORM debug blit pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Renderer context for bindless access.
        Context& m_Context;
        /// @brief The ORM blit pipeline.
        Ref<GraphicsPipeline> m_Pipeline;
        /// @brief Current render extent.
        uvec2 m_Extent;
        /// @brief The ORM channel selector.
        u32 m_Channel;
    };

    /// @brief Fullscreen debug blit of a shadow atlas into the output.
    ///
    /// Off bindless: reaches the shader through a dedicated set-1 (atlas + ordinary sampler,
    /// raw depth). Declares .Sample on the atlas id for the graph-derived barrier.
    class ShadowBlitScenePass final : public ScenePass
    {
    public:
        /// @brief Which atlas to visualize.
        ///
        /// Directional reads io.ShadowMap, Punctual reads io.PunctualShadowMap. The renderer
        /// writes the matching atlas view into binding 0 before Rebuild.
        enum class Source
        {
            Directional,
            Punctual
        };

        /// @brief Constructs the pass.
        /// @param context   Renderer context for bindless access.
        /// @param pipeline  The shadow blit pipeline.
        /// @param extent    Initial render extent; updated via Resize.
        /// @param shadowSet The dedicated set-1 descriptor set (atlas + sampler, raw depth).
        /// @param source    Which atlas this pass visualizes.
        ShadowBlitScenePass(Context& context, Ref<GraphicsPipeline> pipeline, uvec2 extent,
                            Ref<DescriptorSet> shadowSet, Source source)
            : m_Context(context), m_Pipeline(std::move(pipeline)), m_Extent(extent),
              m_ShadowSet(std::move(shadowSet)), m_Source(source)
        {
        }

        /// @brief Updates the render extent.
        void Resize(uvec2 extent) override { m_Extent = extent; }
        /// @brief Contributes the shadow debug blit pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Renderer context for bindless access.
        Context& m_Context;
        /// @brief The shadow blit pipeline.
        Ref<GraphicsPipeline> m_Pipeline;
        /// @brief Current render extent.
        uvec2 m_Extent;
        /// @brief The dedicated set-1 descriptor set.
        Ref<DescriptorSet> m_ShadowSet;
        /// @brief Which atlas this pass visualizes.
        Source m_Source;
    };
}
