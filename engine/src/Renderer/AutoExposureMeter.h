#pragma once

#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Veng.h>

namespace Veng
{
    class AssetManager;
}

namespace Veng::Renderer
{
    class Context;
    class Buffer;
    class ImageView;
    class Sampler;
    class DescriptorSet;
    class DescriptorSetLayout;
    class ComputePipeline;
    class PipelineLayout;
    struct SceneView;

    /// @brief Owns the auto-exposure metering resources and the eye-adaptation state.
    ///
    /// The metering vertical the renderer wires ahead of tonemap: the log-luminance histogram
    /// compute pipeline + its set, a linear sampler, the host-mapped histogram ring, and the
    /// adapted-luminance / resolved-exposure / reset state the readback drives. The metering pass
    /// (Declare) scatters the lit HDR into this frame's ring region; ResolveExposure reads the
    /// region a completed frame wrote, excludes the black bin, trims the percentile band, eases
    /// the adapted luminance, and returns the exposure the tonemap consumes — which the bloom
    /// bright-pass also reads (GetResolvedExposure) to move its display-referred threshold into
    /// HDR units.
    class AutoExposureMeter
    {
    public:
        /// @brief Creates the metering pipeline, ring, sampler, and set (bound to @p hdrView).
        /// @param context The render context the resources are created on.
        /// @param assets  Asset manager used to load the metering compute shader.
        /// @param hdrView The HDR target the metering pass reads; rebindable via RebindHdr.
        /// @return A new AutoExposureMeter.
        static Unique<AutoExposureMeter> Create(Context& context, AssetManager& assets,
                                                const Ref<ImageView>& hdrView);

        /// @brief Destroys the owned resources through the deferred-destruction retire path.
        ~AutoExposureMeter();

        AutoExposureMeter(const AutoExposureMeter&) = delete;
        AutoExposureMeter& operator=(const AutoExposureMeter&) = delete;

        /// @brief Rebuilds the metering set against a recreated HDR target.
        ///
        /// Allocates a fresh set rather than rewriting the old one, which an in-flight frame's
        /// command buffer may still reference (the bindings are not update-after-bind); the
        /// replaced set retires through the deferred-destruction path.
        /// @param hdrView The live HDR target to bind as the metering source.
        void RebindHdr(const Ref<ImageView>& hdrView);

        /// @brief Snaps the next adaptation to the metered value rather than easing toward it.
        ///
        /// Called at construction and whenever the readback ring is stale (a resize or an
        /// enable edge), so the image opens correctly exposed instead of ramping from the seed.
        void RequestReset() { m_Reset = true; }

        /// @brief Resolves this frame's exposure and stores it for the tonemap and bloom reads.
        ///
        /// When @p active, averages the log-luminance histogram a completed frame metered (the
        /// current frame-in-flight's fenced ring region), excludes the black bin, trims the
        /// [low, high] percentile band, eases the internal adapted luminance (eye adaptation),
        /// re-zeroes the region for this frame's pass, and folds the key + SceneView::Exposure
        /// bias into the result. When inactive it is SceneView::Exposure directly.
        /// @param view   The per-frame view carrying the exposure knobs and the delta.
        /// @param active Whether the metering pass is wired this frame.
        /// @return The resolved exposure.
        [[nodiscard]] f32 ResolveExposure(const SceneView& view, bool active);

        /// @brief The exposure the last ResolveExposure resolved (read by the bloom bright-pass).
        [[nodiscard]] f32 GetResolvedExposure() const { return m_ResolvedExposure; }

        /// @brief The host-mapped histogram ring buffer, bound to its import each Execute.
        [[nodiscard]] const Ref<Buffer>& GetHistogramBuffer() const { return m_Buffer; }

        /// @brief Declares the metering compute pass into the graph ahead of tonemap.
        ///
        /// One thread per lit-HDR texel scatters into this frame's log-luminance histogram ring
        /// region (a StorageBufferWrite, so the pass is scheduled and ordered after the HDR write).
        /// @param graph       The renderer's internal graph being rebuilt.
        /// @param hdrId       The HDR target import the pass samples.
        /// @param histogramId The histogram buffer import the pass scatters into.
        /// @param allocExtent The allocation extent, for the dynamic-resolution sub-rect mapping.
        void Declare(RenderGraph& graph, ResourceId hdrId, ResourceId histogramId,
                     uvec2 allocExtent);

    private:
        AutoExposureMeter(Context& context, AssetManager& assets, const Ref<ImageView>& hdrView);

        Context& m_Context;

        /// @brief Frames-in-flight the ring is sized for (derived from the context).
        u32 m_FramesInFlight = 0;

        /// @brief Metering compute pipeline (HDR → log-luminance histogram buffer).
        Ref<ComputePipeline> m_Pipeline;
        /// @brief Layout for the metering pipeline (its set + push block).
        Ref<PipelineLayout> m_Layout;
        /// @brief Set-1 layout for metering: sampled HDR (0) + linear sampler (1) + histogram (2).
        Ref<DescriptorSetLayout> m_SetLayout;
        /// @brief Metering set binding the HDR source, the linear sampler, and the histogram buffer.
        Ref<DescriptorSet> m_Set;
        /// @brief Clamp-to-edge linear sampler the metering pass reads the HDR through.
        Ref<Sampler> m_Sampler;
        /// @brief Host-mapped ring buffer (framesInFlight histogram regions) the metering fills.
        Ref<Buffer> m_Buffer;
        /// @brief Byte stride between the histogram buffer's per-frame ring regions.
        u32 m_Stride = 0;

        /// @brief The internal adapted luminance the exposure derives from; eased toward the meter.
        f32 m_AdaptedLuminance = 0.18f;
        /// @brief The exposure the last ResolveExposure resolved.
        f32 m_ResolvedExposure = 1.0f;
        /// @brief Whether the next adaptation snaps to the metered value rather than easing.
        bool m_Reset = true;
    };
}
