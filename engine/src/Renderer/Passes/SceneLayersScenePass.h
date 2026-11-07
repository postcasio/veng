#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>

namespace Veng::Renderer
{
    /// @brief A layer drawn over the scene's translucent surfaces — ribbons, sprites, particles —
    ///        that can share one render pass with the others.
    ///
    /// Each layer draws into the lit target, the bloom and reactive masks and the read-only opaque
    /// depth, and none samples another's output, so SceneLayersScenePass records every active one
    /// in a single render pass. A layer also declares itself as a pass of its own (Declare), which
    /// is how a placement drawn alone — the post-resolve ribbons — is wired.
    class SceneLayer : public ScenePass
    {
    public:
        /// @brief Declares the work the layer's draw depends on ahead of the shared render pass.
        /// @param graph The renderer's graph being rebuilt.
        /// @param io    The frame's pass IO.
        virtual void DeclareAhead(RenderGraph& /*graph*/, const PassIO& /*io*/) {}

        /// @brief Declares what the draw reads besides the shared attachments.
        /// @param builder The render pass the layer records into.
        /// @param io      The frame's pass IO.
        virtual void DeclareInputs(RenderGraph::PassBuilder& builder, const PassIO& io) const = 0;

        /// @brief Returns whether the layer draws anything this frame.
        [[nodiscard]] virtual bool IsDrawing() const = 0;

        /// @brief Records the layer's draws into the open render pass.
        /// @param ctx The pass context.
        virtual void Record(const ScenePassContext& ctx) const = 0;
    };

    /// @brief The targets every scene layer draws into.
    struct SceneLayerTargets
    {
        /// @brief The lit scene-color target.
        ResourceId Target;
        /// @brief The opaque depth target, bound read-only.
        ResourceId Depth;
        /// @brief The bloom-mask target, or invalid when the frame wires none.
        ResourceId Mask;
        /// @brief The temporal resolve's reactive mask, or invalid when none is wired.
        ResourceId Reactive;
    };

    /// @brief Records the scene's ribbons, sprites and particles, in that order, in one render
    ///        pass.
    ///
    /// The layers load and store the same attachments, so one pass spares the frame a
    /// full-resolution round trip of every attachment per layer. Work a layer's draw depends on (the
    /// particle simulation) is declared ahead of the pass; the pass skips a frame no layer draws in.
    class SceneLayersScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass over the layers it records, in draw order.
        /// @param targets The shared attachments.
        /// @param layers  The wired layers, back to front.
        SceneLayersScenePass(const SceneLayerTargets& targets, vector<Unique<SceneLayer>> layers);

        /// @brief Contributes the layers' prerequisite work and the shared render pass.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief The shared attachments.
        SceneLayerTargets m_Targets;
        /// @brief The wired layers, back to front.
        vector<Unique<SceneLayer>> m_Layers;
    };
}
