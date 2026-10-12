#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>

namespace Veng::Renderer
{
    class Context;
    class GraphicsPipeline;
    class SsrChain;

    /// @brief Places the screen-space reflection chain's passes in the renderer's pass order.
    ///
    /// The chain owns its resources and pipelines; this pass only hands it the g-buffer and the lit
    /// colour the renderer wired, at the position in the pass list its passes are declared.
    class SsrScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass over a chain the renderer owns.
        /// @param chain The chain; must outlive the pass.
        explicit SsrScenePass(SsrChain& chain) : m_Chain(chain) {}

        /// @brief Declares the chain's passes, reading the g-buffer, Velocity and Hdr slots of
        ///        @p io.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief The chain whose passes this declares.
        SsrChain& m_Chain;
    };

    /// @brief The reflection debug arms' terminal blit: one of the chain's outputs to the output.
    class SsrDebugBlitScenePass final : public ScenePass
    {
    public:
        /// @brief Which of the chain's outputs the blit shows.
        enum class Source
        {
            /// @brief The denoised reflection (radiance A), through the plain blit.
            Reflection,
            /// @brief The classifier's coverage codes, through the coverage blit.
            Coverage
        };

        /// @brief Constructs the pass.
        /// @param context  Renderer context for bindless access.
        /// @param chain    The chain whose output is shown; must outlive the pass.
        /// @param pipeline A blit pipeline taking the BlitPushConstants block.
        /// @param extent   Initial render extent; updated via Resize.
        /// @param source   Which output the blit shows.
        SsrDebugBlitScenePass(Context& context, SsrChain& chain, Ref<GraphicsPipeline> pipeline,
                              uvec2 extent, Source source)
            : m_Context(context), m_Chain(chain), m_Pipeline(std::move(pipeline)), m_Extent(extent),
              m_Source(source)
        {
        }

        /// @brief Updates the render extent.
        void Resize(uvec2 extent) override { m_Extent = extent; }

        /// @brief Declares the blit after the chain's passes, writing the Output slot of @p io.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Renderer context for bindless access.
        Context& m_Context;
        /// @brief The chain whose output is shown.
        SsrChain& m_Chain;
        /// @brief The blit pipeline.
        Ref<GraphicsPipeline> m_Pipeline;
        /// @brief The render extent.
        uvec2 m_Extent;
        /// @brief Which output the blit shows.
        Source m_Source;
    };
}
