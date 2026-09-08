#pragma once

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/SceneRendererSettings.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Veng.h>

namespace Veng
{
    class AssetManager;
}

namespace Veng::Renderer
{
    class Context;
    class Image;
    class ImageView;
    class GraphicsPipeline;
    class PipelineLayout;

    /// @brief Owns the post-tonemap spatial anti-aliasing vertical (FXAA and CMAA2).
    ///
    /// The two spatial resolves share one input: the tonemapped LDR image the renderer routes the
    /// tonemap into (instead of the output) when either is active. This subsystem owns that
    /// intermediate target and its bindless slot, plus the FXAA pipeline. The pass that consumes
    /// them is FxaaScenePass, which the renderer constructs from what this exposes; the temporal
    /// resolve (TAA) is unrelated and lives in TaaResolve. Nothing here is allocated until a
    /// post-tonemap mode is active, so the shipping path holds no extra memory.
    class AaResolve
    {
    public:
        /// @brief Creates the FXAA pipeline (the input target is built by Resize).
        /// @param context      The render context the resources are created on.
        /// @param assets       Asset manager used to load the FXAA fragment shader.
        /// @param outputFormat The color format the resolve writes and the intermediate carries.
        /// @return A new AaResolve.
        static Unique<AaResolve> Create(Context& context, AssetManager& assets,
                                        Format outputFormat);

        /// @brief Releases the input bindless slot; the image retires through the frame bin.
        ~AaResolve();

        AaResolve(const AaResolve&) = delete;
        AaResolve& operator=(const AaResolve&) = delete;

        /// @brief Recreates the LDR input target at @p extent, or releases it when no spatial AA runs.
        ///
        /// The intermediate is the output format at the full allocation extent, registered into
        /// bindless when @p mode is a post-tonemap mode (FXAA or CMAA2); otherwise it is dropped so
        /// the memory is not held for an unused path.
        /// @param extent The allocation extent the intermediate is sized to.
        /// @param mode   The active anti-aliasing mode (the target is allocated only for FXAA/CMAA2).
        void Resize(uvec2 extent, AntiAliasingMode mode);

        /// @brief The FXAA resolve pipeline (luma-directed edge blur), writing the output format.
        [[nodiscard]] const Ref<GraphicsPipeline>& GetFxaaPipeline() const
        {
            return m_FxaaPipeline;
        }

        /// @brief The CMAA2 edge-detection pipeline (writes the RG8 edge map).
        [[nodiscard]] const Ref<GraphicsPipeline>& GetCmaa2EdgePipeline() const
        {
            return m_Cmaa2EdgePipeline;
        }

        /// @brief The CMAA2 apply pipeline (edge-directed blend), writing the output format.
        [[nodiscard]] const Ref<GraphicsPipeline>& GetCmaa2ApplyPipeline() const
        {
            return m_Cmaa2ApplyPipeline;
        }

        /// @brief The LDR intermediate the tonemap writes and the resolve reads; null when off.
        [[nodiscard]] const Ref<ImageView>& GetInputView() const { return m_InputView; }

        /// @brief Bindless slot for the LDR intermediate; the resolve samples the tonemapped scene.
        [[nodiscard]] TextureHandle GetInputHandle() const { return m_InputHandle; }

        /// @brief The CMAA2 edge map (RG8: right/bottom edges); null unless CMAA2 is active.
        [[nodiscard]] const Ref<ImageView>& GetEdgeView() const { return m_EdgeView; }

        /// @brief Bindless slot for the edge map; the apply pass follows it.
        [[nodiscard]] TextureHandle GetEdgeHandle() const { return m_EdgeHandle; }

    private:
        AaResolve(Context& context, AssetManager& assets, Format outputFormat);

        Context& m_Context;
        /// @brief The color format the intermediate carries and the resolve writes.
        Format m_OutputFormat;

        /// @brief The FXAA pipeline (fullscreen triangle → fxaa.frag), writing the output format.
        Ref<GraphicsPipeline> m_FxaaPipeline;
        /// @brief Layout for the FXAA and CMAA2-edge pipelines: the texture + sampler + rcpFrame push.
        Ref<PipelineLayout> m_FxaaLayout;

        /// @brief The CMAA2 edge-detection pipeline (fullscreen → cmaa2_edges.frag, writes RG8 edges).
        Ref<GraphicsPipeline> m_Cmaa2EdgePipeline;
        /// @brief The CMAA2 apply pipeline (fullscreen → cmaa2_apply.frag, writes the output).
        Ref<GraphicsPipeline> m_Cmaa2ApplyPipeline;
        /// @brief Layout for the CMAA2 apply pipeline: the color + edge + sampler + rcpFrame push.
        Ref<PipelineLayout> m_Cmaa2ApplyLayout;

        /// @brief The tonemapped LDR intermediate the resolve reads (allocated only under FXAA/CMAA2).
        Ref<Image> m_InputImage;
        /// @brief View over m_InputImage.
        Ref<ImageView> m_InputView;
        /// @brief Bindless slot for the input view.
        TextureHandle m_InputHandle;

        /// @brief The CMAA2 edge map (RG8, right/bottom edges), allocated only under CMAA2.
        Ref<Image> m_EdgeImage;
        /// @brief View over m_EdgeImage.
        Ref<ImageView> m_EdgeView;
        /// @brief Bindless slot for the edge view.
        TextureHandle m_EdgeHandle;
    };
}
