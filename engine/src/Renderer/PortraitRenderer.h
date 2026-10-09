#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/ModelPortrait.h>

namespace Veng
{
    class AssetManager;
}

namespace Veng::Renderer
{
    class CommandBuffer;
    class Context;
    class GraphicsPipeline;
    class Image;
    class ImageView;
    class PipelineLayout;
    class SceneRenderer;
    struct SceneView;

    /// @brief The lean renderer a ModelPortrait draws through, pooled by configuration.
    ///
    /// Owns a SceneRenderer at the portrait's extent — the full lean deferred pipeline for Shaded,
    /// the GeometryDepthNormal path otherwise — and, for Shaded, a colour target its coverage pass
    /// writes: the tonemapped output with alpha taken from depth, so the model composites over
    /// whatever a UI draws behind it. Every bindless slot it registers is released by its destructor.
    class PortraitRenderer
    {
    public:
        /// @brief Builds the renderer, its colour target and coverage pipeline for @p config.
        /// @param context  The render context it allocates on.
        /// @param assets   The asset manager its shaders load through.
        /// @param config   Its size and output.
        PortraitRenderer(Context& context, AssetManager& assets,
                         const PortraitRendererConfig& config);

        /// @brief Releases the slots it registered.
        ~PortraitRenderer();

        PortraitRenderer(const PortraitRenderer&) = delete;
        PortraitRenderer& operator=(const PortraitRenderer&) = delete;

        /// @brief Returns the configuration it was built for.
        [[nodiscard]] const PortraitRendererConfig& GetConfig() const { return m_Config; }

        /// @brief Drops the scene its renderer last gathered and reads as never rendered.
        ///
        /// Run before it is pooled, and before the scene it drew is destroyed.
        void ResetForReuse();

        /// @brief Renders @p view and leaves the outputs in a sampled layout.
        /// @param cmd   The frame command buffer.
        /// @param view  The private scene and camera to render.
        void Render(CommandBuffer& cmd, const SceneView& view);

        /// @brief Whether Render has run since it was built or reset.
        [[nodiscard]] bool HasRendered() const { return m_Rendered; }

        /// @brief Returns the Shaded colour output's handle; invalid on the GeometryDepthNormal path.
        [[nodiscard]] TextureHandle GetColorHandle() const { return m_ColorHandle; }

        /// @brief Returns the GeometryDepthNormal depth handle; invalid on the Shaded path.
        [[nodiscard]] TextureHandle GetDepthHandle() const;

        /// @brief Returns the GeometryDepthNormal normal handle; invalid on the Shaded path.
        [[nodiscard]] TextureHandle GetNormalHandle() const;

        /// @brief Returns the clamped linear sampler the colour output is read through.
        [[nodiscard]] SamplerHandle GetSampler() const { return m_Sampler; }

        /// @brief Returns the Shaded colour target's view, or null on the GeometryDepthNormal path.
        [[nodiscard]] const Ref<ImageView>& GetColorView() const { return m_ColorView; }

        /// @brief Returns the scene renderer, for diagnostics and tests.
        [[nodiscard]] const SceneRenderer& GetSceneRenderer() const { return *m_Renderer; }

    private:
        /// @brief The render context.
        Context& m_Context;
        /// @brief The size and output it was built for.
        PortraitRendererConfig m_Config;
        /// @brief The scene renderer at the portrait's extent.
        Unique<SceneRenderer> m_Renderer;
        /// @brief Bindless slot of the renderer's tonemapped output, read by the coverage pass.
        TextureHandle m_OutputHandle;
        /// @brief The Shaded colour target the coverage pass writes.
        Ref<Image> m_Color;
        /// @brief View over m_Color.
        Ref<ImageView> m_ColorView;
        /// @brief Bindless slot of m_ColorView — the portrait's Color output.
        TextureHandle m_ColorHandle;
        /// @brief The shared clamped linear sampler handed out with the outputs.
        SamplerHandle m_Sampler;
        /// @brief The coverage pass's layout.
        Ref<PipelineLayout> m_CoverageLayout;
        /// @brief The coverage pass's pipeline.
        Ref<GraphicsPipeline> m_CoveragePipeline;
        /// @brief Whether Render has run since construction or ResetForReuse.
        bool m_Rendered = false;
    };

    /// @brief Hands back a portrait's renderer when it no longer matches its Extent or Output.
    ///
    /// The compositor runs it ahead of materializing, so a resized or re-targeted portrait takes a
    /// renderer of its new configuration (from the pool, or built within the frame's budget).
    /// @param portrait  The portrait to check.
    void ReleaseMismatchedRenderer(const ModelPortrait& portrait);
}
