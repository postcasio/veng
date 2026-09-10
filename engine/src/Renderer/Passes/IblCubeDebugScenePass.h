#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    class Context;
    class GraphicsPipeline;
    class DescriptorSet;
    class DescriptorSetLayout;
    class ImageView;
    class Sampler;

    /// @brief Terminal debug pass: fills the whole output with a radiance cube sampled along each
    ///        view ray.
    ///
    /// Serves the DebugView::EnvironmentIrradiance arm (the diffuse irradiance cube the IBL lighting's
    /// diffuse term reads) and the DebugView::EnvironmentSource arm (the raw cube the IBL convolved
    /// from). Unlike
    /// the skybox it draws every pixel — the environment lighting the scene is visible whatever
    /// geometry is in front of it — and it writes the output target directly with no tonemap tail.
    ///
    /// The renderer feeds the arm's cube view + sampler + LOD each frame through SetCube before the
    /// graph records; the pass rebuilds its dedicated set (set 3) only when the cube or sampler
    /// changes, so a stable source records no descriptor writes. A null cube shows black.
    class IblCubeDebugScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass.
        /// @param context   The render context.
        /// @param pipeline  The fullscreen cube-blit pipeline (reserves sets 0-2 bindless + set 3 cube).
        /// @param setLayout The cube set layout (binding 0 cube, binding 1 sampler) sets are built on.
        /// @param extent    The render extent.
        IblCubeDebugScenePass(Context& context, Ref<GraphicsPipeline> pipeline,
                              Ref<DescriptorSetLayout> setLayout, uvec2 extent);

        /// @brief Sets the cube the pass samples this frame, rebuilding the set only on a change.
        ///
        /// The cube is always bound (the shader statically declares set 3, so a valid set must be
        /// bound even when the arm shows black); `enabled` false draws black without sampling it, so
        /// a source arm with no backing cube passes a valid fallback cube here with enabled false.
        /// @param cube    The cube view to bind; never null.
        /// @param sampler The linear sampler to read it through.
        /// @param lod     The mip to sample (prefilter roughness LOD; 0 for a single-mip cube).
        /// @param enabled Whether a cube backs this arm; false shows black.
        void SetCube(const Ref<ImageView>& cube, const Ref<Sampler>& sampler, f32 lod,
                     bool enabled);

        /// @brief Updates the cached render extent.
        void Resize(uvec2 extent) override { m_Extent = extent; }

        /// @brief Declares the fullscreen cube blit into the output target.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        Context& m_Context;
        Ref<GraphicsPipeline> m_Pipeline;
        Ref<DescriptorSetLayout> m_SetLayout;
        Ref<DescriptorSet> m_Set;

        // The cube + sampler the set was last built against, so SetCube rebuilds only on a change.
        const ImageView* m_SetCube = nullptr;
        const Sampler* m_SetSampler = nullptr;

        bool m_Enabled = false; // a cube backs this arm this frame
        f32 m_Lod = 0.0f;
        uvec2 m_Extent;
    };
}
