#pragma once

#include <Veng/Veng.h>

#include "Backend/MetalFx.h"
#include "FrameTopology.h"

namespace Veng
{
    class AssetManager;
}

namespace Veng::Renderer
{
    class Context;
    class GraphicsPipeline;
    class Image;
    class ImageView;
    class PipelineLayout;

    /// @brief The motion pass's push block, matching metalfx_motion.frag PushConstants.
    struct MetalFxMotionPush
    {
        /// @brief The g-buffer depth's bindless slot.
        u32 DepthTexture;
        /// @brief The g-buffer velocity channel's bindless slot.
        u32 VelocityTexture;
        /// @brief The current frame-in-flight's view-constants region.
        u32 ViewConstantsIndex;
    };

    /// @brief Owns the renderer's MetalFX scalers and what the device lets them do.
    ///
    /// The MetalFX vertical beside TaaResolve and PostResolveUpscale: the device's support, read
    /// once; the temporal scaler a MetalFXTemporal frame resolves through and the spatial one a
    /// MetalFXSpatial promotion upscales through, each sized to the allocations it bridges and
    /// held only while its mode is wired; and the record of a shape MetalFX refused, which the
    /// settings resolution reads so the renderer falls back rather than renders nothing. Beside
    /// the temporal scaler it holds the motion target the scaler reads — the velocity channel with
    /// the background's motion filled in from the camera's turn — and the pipeline that writes it.
    /// The passes are MetalFxMotionScenePass, MetalFxTemporalScenePass and MetalFxSpatialScenePass.
    class MetalFxUpscale
    {
    public:
        /// @brief Reads the device's MetalFX support and builds the motion pipeline; creates no
        ///        scaler.
        /// @param context The render context the scalers are created on.
        /// @param assets  Asset manager the motion pass's shaders load through.
        MetalFxUpscale(Context& context, AssetManager& assets);

        /// @brief Drops the scalers and the motion target; each retires through the frame bin.
        ~MetalFxUpscale();

        MetalFxUpscale(const MetalFxUpscale&) = delete;
        MetalFxUpscale& operator=(const MetalFxUpscale&) = delete;

        /// @brief What MetalFX can run for the renderer's current allocations.
        /// @return Each scaler's support, less any shape it refused since the last ClearRefusals.
        [[nodiscard]] MetalFxAvailability GetAvailability() const
        {
            return {
                .Temporal = m_Support.Temporal && !m_TemporalRefused,
                .Spatial = m_Support.Spatial && !m_SpatialRefused,
            };
        }

        /// @brief Forgets the shapes MetalFX refused, so the next allocations are tried afresh.
        void ClearRefusals()
        {
            m_TemporalRefused = false;
            m_SpatialRefused = false;
        }

        /// @brief Recreates the temporal scaler for the given allocations, or drops it.
        ///
        /// A scaler of the same shape is kept, so its history survives a reconfigure that moved
        /// neither allocation. A shape MetalFX refuses is recorded, and GetAvailability reports the
        /// temporal scaler unavailable until ClearRefusals.
        /// @param input   The render allocation the jittered colour, depth and velocity fill.
        /// @param output  The scene-colour allocation the scaler reconstructs.
        /// @param enabled Whether the temporal scaler is wired.
        /// The motion target follows it, sized to @p input and held only while a scaler is.
        /// @return False when the scaler was wanted and MetalFX refused the shape.
        bool ResizeTemporal(uvec2 input, uvec2 output, bool enabled);

        /// @brief Recreates the spatial scaler for the given allocations, or drops it.
        ///
        /// Same keep-and-refuse behaviour as ResizeTemporal.
        /// @param input   The scene-colour allocation the promotion reads.
        /// @param output  The post-resolve allocation the promotion writes.
        /// @param enabled Whether the spatial promotion is wired.
        /// @return False when the scaler was wanted and MetalFX refused the shape.
        bool ResizeSpatial(uvec2 input, uvec2 output, bool enabled);

        /// @brief The smallest per-frame render scale the temporal scaler can reconstruct from.
        ///
        /// The scaler accepts content down to OutputExtent / TemporalMaxScale, so a
        /// dynamic-resolution frame rendering less would hand it a sub-rect it cannot read.
        /// @return The render-scale floor, or 0 while no temporal scaler is held.
        [[nodiscard]] f32 GetTemporalRenderScaleFloor() const;

        /// @brief The temporal scaler; null while not wired.
        [[nodiscard]] Backend::MetalFxTemporalScaler* GetTemporal() const
        {
            return m_Temporal.get();
        }

        /// @brief The spatial scaler; null while not wired.
        [[nodiscard]] Backend::MetalFxSpatialScaler* GetSpatial() const { return m_Spatial.get(); }

        /// @brief The motion pass's pipeline, writing the motion target's format.
        [[nodiscard]] const Ref<GraphicsPipeline>& GetMotionPipeline() const
        {
            return m_MotionPipeline;
        }

        /// @brief The motion target the temporal scaler reads; null while no scaler is held.
        [[nodiscard]] const Ref<ImageView>& GetMotionView() const { return m_MotionView; }

    private:
        Context& m_Context;
        /// @brief The device's MetalFX support, read at construction.
        Backend::MetalFxSupport m_Support;
        /// @brief The temporal scaler, while MetalFXTemporal is wired.
        Unique<Backend::MetalFxTemporalScaler> m_Temporal;
        /// @brief The spatial scaler, while the MetalFXSpatial promotion is wired.
        Unique<Backend::MetalFxSpatialScaler> m_Spatial;
        /// @brief MetalFX refused the temporal shape last asked for.
        bool m_TemporalRefused = false;
        /// @brief MetalFX refused the spatial shape last asked for.
        bool m_SpatialRefused = false;

        /// @brief Recreates the motion target at @p extent, or releases it.
        void ResizeMotion(uvec2 extent, bool enabled);

        /// @brief Layout for the motion pipeline: the motion push block.
        Ref<PipelineLayout> m_MotionLayout;
        /// @brief The motion pass's pipeline.
        Ref<GraphicsPipeline> m_MotionPipeline;
        /// @brief The motion target, the render allocation in the velocity channel's format.
        Ref<Image> m_MotionImage;
        /// @brief View over m_MotionImage.
        Ref<ImageView> m_MotionView;
    };
}
