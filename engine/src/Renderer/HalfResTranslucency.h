#pragma once

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/RenderGraph.h>
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
    class ScenePass;
    struct TranslucentDrawPlan;

    /// @brief The half-resolution extent a full-resolution extent reduces to.
    ///
    /// Rounds up, floored at one texel, so an odd extent's last row/column still has a
    /// half-res texel covering it. Every consumer of the layer — the targets, the viewport,
    /// the reduce, the composite's tap window, the view-constants extents — derives its
    /// half extent through this one function, so they cannot disagree by a rounding rule.
    inline uvec2 HalfResExtent(const uvec2 extent)
    {
        return {std::max((extent.x + 1u) / 2u, 1u), std::max((extent.y + 1u) / 2u, 1u)};
    }

    /// @brief Owns the reduced-resolution translucent layer — its targets, pipelines, and passes.
    ///
    /// Translucent materials that opt in at authoring time (Material::IsHalfResolution) draw
    /// into a half-resolution HDR layer instead of the full-resolution translucent pass, paying
    /// a quarter of the fragment cost — the trade a smooth, screen-filling volumetric surface
    /// (an atmosphere shell, a fog bank) makes gladly and a crisp-edged surface does not. The
    /// layer is three passes wired immediately ahead of the full translucent pass, so its result
    /// composites under every full-resolution translucent draw:
    ///
    /// 1. A depth reduce writes the half depth target as the farthest (reverse-Z minimum) of
    ///    each texel's 2x2 full-res opaque depths, so the half draws depth-test conservatively.
    /// 2. A half-viewport TranslucentScenePass draws the routed materials back-to-front over a
    ///    transparent clear; the straight alpha blend leaves (premultiplied color, coverage).
    /// 3. A fullscreen composite upsamples the layer depth-aware (bilinear weights collapsed
    ///    across depth mismatches, so the layer hugs geometry edges) and blends it into the lit
    ///    scene color with (One, OneMinusSrcAlpha).
    ///
    /// The layer is content-driven: the renderer activates it the first frame the translucent
    /// gather routes a draw into the half plan and drops it when the last one goes, so a frame
    /// with no opted-in material carries no targets and no passes.
    class HalfResTranslucency
    {
    public:
        /// @brief Creates the reduce and composite pipelines (the targets are built by Recreate).
        /// @param context The render context the resources are created on.
        /// @param assets  Asset manager used to load the reduce and composite fragment shaders.
        /// @return A new HalfResTranslucency.
        static Unique<HalfResTranslucency> Create(Context& context, AssetManager& assets);

        /// @brief Releases the layer's bindless slots; the images retire through the frame bin.
        ~HalfResTranslucency();

        HalfResTranslucency(const HalfResTranslucency&) = delete;
        HalfResTranslucency& operator=(const HalfResTranslucency&) = delete;

        /// @brief Recreates the half-resolution color and depth targets for the current extent.
        ///
        /// Allocates the HalfResExtent-sized HDR layer target and D32 reduced-depth target and
        /// registers their bindless slots when @p active; otherwise releases any previously
        /// created pair. Called from the renderer's Resize/Configure with the current activation
        /// and from the activation edge itself.
        /// @param active Whether the layer is active (content routed a draw into it).
        /// @param extent The full-resolution allocation extent the half targets derive from.
        void Recreate(bool active, uvec2 extent);

        /// @brief Contributes the layer's three passes into the pass list.
        ///
        /// Appended immediately ahead of the full-resolution translucent pass in both compositing
        /// Rebuild arms, so the layer blends under every full-resolution translucent draw.
        /// @param passes       The renderer's pass list to append into.
        /// @param layerId      The half-res layer color target import id.
        /// @param halfDepthId  The half-res reduced depth target import id.
        /// @param depthId      The full-res opaque depth id (reduce source, composite reference).
        /// @param depthHandle  Bindless slot for the full-res opaque depth.
        /// @param targetId     The lit scene-color target the composite blends into.
        /// @param plan         Borrowed per-frame half-res translucent draw plan.
        /// @param sceneColorId Refraction scene-color intermediate, or invalid when off.
        /// @param sceneDepthId Refraction depth intermediate, or invalid when off.
        /// @param extent       The full-resolution allocation extent.
        void Declare(vector<Unique<ScenePass>>& passes, ResourceId layerId, ResourceId halfDepthId,
                     ResourceId depthId, TextureHandle depthHandle, ResourceId targetId,
                     const TranslucentDrawPlan* plan, ResourceId sceneColorId,
                     ResourceId sceneDepthId, uvec2 extent) const;

        /// @brief The half-res layer color view (bound to its import while the layer is active).
        [[nodiscard]] const Ref<ImageView>& GetLayerView() const { return m_LayerView; }

        /// @brief The half-res reduced depth view (bound to its import while the layer is active).
        [[nodiscard]] const Ref<ImageView>& GetDepthView() const { return m_DepthView; }

    private:
        HalfResTranslucency(Context& context, AssetManager& assets);

        Context& m_Context;

        /// @brief The depth-reduce pipeline: fullscreen, no color, SV_Depth into the half target.
        Ref<GraphicsPipeline> m_ReducePipeline;
        /// @brief Layout for m_ReducePipeline: a push block of the source slot and valid extent.
        Ref<PipelineLayout> m_ReduceLayout;
        /// @brief The composite pipeline: fullscreen, (One, OneMinusSrcAlpha) into the lit color.
        Ref<GraphicsPipeline> m_CompositePipeline;
        /// @brief Layout for m_CompositePipeline: a push block of the three slots and extents.
        Ref<PipelineLayout> m_CompositeLayout;

        /// @brief The half-resolution layer color target the routed translucents draw into.
        Ref<Image> m_LayerImage;
        /// @brief View over m_LayerImage.
        Ref<ImageView> m_LayerView;
        /// @brief Bindless slot for m_LayerView (the composite's read).
        TextureHandle m_LayerHandle;
        /// @brief The half-resolution reduced opaque depth the layer depth-tests against.
        Ref<Image> m_DepthImage;
        /// @brief Sampled view over m_DepthImage.
        Ref<ImageView> m_DepthView;
        /// @brief Bindless slot for m_DepthView (the composite's edge reference).
        TextureHandle m_DepthHandle;
    };
}
