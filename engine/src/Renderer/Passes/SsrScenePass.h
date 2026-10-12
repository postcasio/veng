#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>

namespace Veng::Renderer
{
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

        /// @brief Declares the chain's passes, reading the g-buffer and Hdr slots of @p io.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief The chain whose passes this declares.
        SsrChain& m_Chain;
    };
}
