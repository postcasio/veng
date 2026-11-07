#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/Types.h>

#include "SceneLayersScenePass.h"
#include "TranslucentScenePass.h"

namespace Veng
{
    class AssetManager;
}

namespace Veng::Renderer
{
    class Context;
    class GraphicsPipeline;
    class ParticleSimulation;
    class PipelineLayout;

    /// @brief Construction parameters for a ParticleScenePass.
    struct ParticleScenePassInfo
    {
        /// @brief The renderer's particle battery, whose chain the pass declares ahead of its draw.
        ParticleSimulation* Simulation = nullptr;
        /// @brief The lit scene-color target particles composite into.
        ResourceId Target;
        /// @brief The opaque depth target, bound read-only for depth-testing.
        ResourceId Depth;
        /// @brief The bloom-mask target, or invalid when the frame wires none.
        ResourceId Mask;
        /// @brief The temporal resolve's reactive mask, or invalid when no temporal resolve is
        ///        wired.
        ResourceId Reactive;
        /// @brief The opaque-depth copy a soft particle reads, or invalid when the frame has none.
        ResourceId SceneDepthCopy;
        /// @brief The renderer's IBL and shadow sets, which the chain's update binds.
        ForwardLightingSets Forward;
        /// @brief Color format of the scene-color target.
        Format TargetFormat = Format::Undefined;
        /// @brief Color format of the bloom-mask target.
        Format MaskFormat = Format::Undefined;
        /// @brief Color format of the reactive mask.
        Format ReactiveFormat = Format::Undefined;
    };

    /// @brief Simulates and draws the frame's particles into the lit scene color.
    ///
    /// Declares the battery's compute chain (ParticleSimulation::Declare) and then "Scene
    /// Particles", which draws the update's records with two indirect draws: the alpha set in its
    /// sorted back-to-front order, premultiplied-over, then the additive set. Each particle is a
    /// quad expanded in the vertex stage from its record (no vertex input), depth-tested against
    /// the opaque depth with depth writes off. Wired immediately after the sprite pass, so alpha
    /// particles composite over sprites, and ahead of TAA, bloom and tonemap. When the frame wires
    /// a bloom mask, an unlit billboard or a streak writes it by its luminance and a six-way one by
    /// its emission's.
    class ParticleScenePass final : public SceneLayer
    {
    public:
        /// @brief Constructs the pass and its pipelines.
        /// @param context  Renderer context.
        /// @param assets   The asset manager the core particle shaders load through.
        /// @param info     The battery, targets, forward sets and formats.
        ParticleScenePass(Context& context, AssetManager& assets,
                          const ParticleScenePassInfo& info);

        /// @brief Releases the pipelines.
        ~ParticleScenePass() override;

        /// @brief Contributes the chain and the draw, as a pass of its own, into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

        /// @brief Declares the simulation chain the draw reads.
        void DeclareAhead(RenderGraph& graph, const PassIO& io) override;

        /// @brief Declares the opaque-depth copy and the chain's draw buffers read.
        void DeclareInputs(RenderGraph::PassBuilder& builder, const PassIO& io) const override;

        /// @brief Returns whether the chain draws this frame.
        [[nodiscard]] bool IsDrawing() const override;

        /// @brief Records the two indirect draws.
        void Record(const ScenePassContext& ctx) const override;

    private:
        /// @brief Renderer context.
        Context& m_Context;
        /// @brief The battery.
        ParticleSimulation* m_Simulation;
        /// @brief The lit scene-color target.
        ResourceId m_TargetId;
        /// @brief The opaque depth target.
        ResourceId m_DepthId;
        /// @brief The bloom-mask target (invalid when the frame wires none).
        ResourceId m_MaskId;
        /// @brief The reactive mask (invalid when no temporal resolve is wired).
        ResourceId m_ReactiveId;
        /// @brief The opaque-depth copy (invalid when the frame has none).
        ResourceId m_SceneDepthCopyId;
        /// @brief The renderer's IBL and shadow sets.
        ForwardLightingSets m_Forward;
        /// @brief The draw's layout.
        Ref<PipelineLayout> m_Layout;
        /// @brief The premultiplied-over pipeline.
        Ref<GraphicsPipeline> m_AlphaPipeline;
        /// @brief The additive pipeline.
        Ref<GraphicsPipeline> m_AdditivePipeline;
    };
}
