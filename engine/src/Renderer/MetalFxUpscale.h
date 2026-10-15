#pragma once

#include <Veng/Veng.h>

#include "Backend/MetalFx.h"
#include "FrameTopology.h"

namespace Veng::Renderer
{
    class Context;

    /// @brief Owns the renderer's MetalFX scalers and what the device lets them do.
    ///
    /// The MetalFX vertical beside TaaResolve and PostResolveUpscale: the device's support, read
    /// once; the temporal scaler a MetalFXTemporal frame resolves through and the spatial one a
    /// MetalFXSpatial promotion upscales through, each sized to the allocations it bridges and
    /// held only while its mode is wired; and the record of a shape MetalFX refused, which the
    /// settings resolution reads so the renderer falls back rather than renders nothing. The passes
    /// are MetalFxTemporalScenePass and MetalFxSpatialScenePass.
    class MetalFxUpscale
    {
    public:
        /// @brief Reads the device's MetalFX support; creates no scaler.
        /// @param context The render context the scalers are created on.
        explicit MetalFxUpscale(Context& context);

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
    };
}
