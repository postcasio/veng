#pragma once

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Veng.h>

#include "Passes/TranslucentScenePass.h"

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

    /// @brief How far a segment's composite reaches past its draws' projected rectangle, in pixels.
    ///
    /// The composite's depth-aware upsample reads a 2x2 footprint of half-resolution texels, and a
    /// draw rasterizes into the half texels its edge touches, so the layer can hold coverage a
    /// couple of full-resolution pixels outside the rectangle.
    inline constexpr u32 HalfResCompositeMarginPixels = 4;

    /// @brief A pixel rectangle: an offset and an extent.
    struct HalfResScissor
    {
        /// @brief The rectangle's top-left pixel.
        ivec2 Offset{0};
        /// @brief The rectangle's size in pixels.
        uvec2 Extent{0};
    };

    /// @brief The pixels a segment's composite covers: its UV rectangle over the valid extent,
    ///        rounded outward and grown by HalfResCompositeMarginPixels, clamped to the extent.
    /// @param rectUv       The segment's screen rectangle (min xy, max xy) in UV.
    /// @param validExtent  The frame's rendered sub-rect, in pixels.
    /// @return The scissor rectangle.
    [[nodiscard]] inline HalfResScissor HalfResCompositeScissor(const vec4 rectUv,
                                                                const uvec2 validExtent)
    {
        const vec2 extent{validExtent};
        const auto margin = static_cast<f32>(HalfResCompositeMarginPixels);
        const vec2 lo = glm::clamp(glm::floor(vec2(rectUv) * extent) - margin, vec2(0.0f), extent);
        const vec2 hi =
            glm::clamp(glm::ceil(vec2(rectUv.z, rectUv.w) * extent) + margin, vec2(0.0f), extent);
        return {.Offset = ivec2(lo), .Extent = uvec2(glm::max(hi - lo, vec2(0.0f)))};
    }

    /// @brief Owns the reduced-resolution translucent layer — its targets, pipelines, and passes.
    ///
    /// Translucent materials that opt in at authoring time (Material::IsHalfResolution) draw
    /// into a half-resolution HDR layer instead of the full-resolution translucent pass, paying
    /// a quarter of the fragment cost — the trade a smooth, screen-filling volumetric surface
    /// (an atmosphere shell, a fog bank) makes gladly and a crisp-edged surface does not. The
    /// layer composites at its place among the full-resolution translucents, one segment per run of
    /// half-res draws (see TranslucentSegments):
    ///
    /// 1. A depth reduce, once, writes the half depth target as the farthest (reverse-Z minimum)
    ///    of each texel's 2x2 full-res opaque depths, so the half draws depth-test conservatively.
    /// 2. Per segment, a half-viewport TranslucentScenePass draws the run back-to-front over a
    ///    transparent clear; the straight alpha blend leaves (premultiplied color, coverage).
    /// 3. Per segment, a fullscreen composite scissored to the run's rectangle upsamples the layer
    ///    depth-aware (bilinear weights collapsed across depth mismatches, so the layer hugs
    ///    geometry edges) and blends it into the lit scene color with (One, OneMinusSrcAlpha).
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

        /// @brief Contributes the depth reduce every segment's layer depth-tests against.
        ///
        /// Appended once, after the first full-resolution translucent pass and ahead of the first
        /// segment. It skips a frame whose first segment drew nothing, which is every frame with no
        /// half-resolution draw.
        /// @param passes       The renderer's pass list to append into.
        /// @param halfDepthId  The half-res reduced depth target import id.
        /// @param depthId      The full-res opaque depth id the reduce reads.
        /// @param depthHandle  Bindless slot for the full-res opaque depth.
        /// @param firstSegment The first segment's half-res draw plan.
        void DeclareDepthReduce(vector<Unique<ScenePass>>& passes, ResourceId halfDepthId,
                                ResourceId depthId, TextureHandle depthHandle,
                                const TranslucentDrawPlan* firstSegment) const;

        /// @brief Contributes one segment's layer pass and its composite.
        ///
        /// The layer is cleared and drawn with the segment's plan, then composited into the lit
        /// scene color over everything the passes before it drew, scissored to @p rect. Both skip a
        /// frame whose segment drew nothing.
        /// @param passes       The renderer's pass list to append into.
        /// @param segment      The segment's index, which names its passes.
        /// @param layerId      The half-res layer color target import id.
        /// @param halfDepthId  The half-res reduced depth target import id.
        /// @param depthId      The full-res opaque depth id (the composite's edge reference).
        /// @param depthHandle  Bindless slot for the full-res opaque depth.
        /// @param targetId     The lit scene-color target the composite blends into.
        /// @param plan         Borrowed per-frame draw plan of this segment's half-res draws.
        /// @param rect         Borrowed per-frame screen rectangle of those draws, in UV.
        /// @param sceneColorId Refraction scene-color intermediate, or invalid when off.
        /// @param sceneDepthId Refraction depth intermediate, or invalid when off.
        /// @param extent       The full-resolution allocation extent.
        /// @param forward      The IBL and shadow sets a forward-lit material's draws bind.
        /// @param pipelines    The pipeline cache every segment's layer pass shares.
        void DeclareSegment(vector<Unique<ScenePass>>& passes, u32 segment, ResourceId layerId,
                            ResourceId halfDepthId, ResourceId depthId, TextureHandle depthHandle,
                            ResourceId targetId, const TranslucentDrawPlan* plan, const vec4* rect,
                            ResourceId sceneColorId, ResourceId sceneDepthId, uvec2 extent,
                            const ForwardLightingSets& forward,
                            const Ref<TranslucentScenePass::PipelineCache>& pipelines) const;

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
