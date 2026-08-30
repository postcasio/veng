#pragma once

#include <Veng/Veng.h>

/// @brief The geometry a FlowField transports a dye through: its wrap, its step, its metric.
///
/// Factored out of FlowFieldInfo so a caller can build the grid description once and reuse it,
/// and so the device-free reference math shares one vocabulary with the shaders.
namespace Veng::Renderer
{
    /// @brief How a grid axis behaves at its edges.
    enum class FlowWrap : u8
    {
        /// @brief The axis is a ring: a coordinate past one edge folds around to the other.
        Periodic,
        /// @brief The axis is bounded: a coordinate past an edge clamps to the outermost texel.
        Clamped,
    };

    /// @brief How the advect's back-trace samples the source field.
    enum class FlowAdvectFilter : u8
    {
        /// @brief The four-tap bilinear read — the default and the cheapest.
        Bilinear,
        /// @brief A sixteen-tap Catmull-Rom read, clamped to its inner four taps' range.
        ///
        /// For an advect run in many small sub-texel steps — a slowed live field — where
        /// bilinear's per-resample blur outruns the transport: the cubic's negative lobes restore
        /// what bilinear smears, and the clamp keeps their overshoot inside the neighbourhood's
        /// own values. Four times the taps of the bilinear read.
        CatmullRom,
    };

    /// @brief The grid geometry a FlowField advects through — no images, no device state.
    struct FlowFieldShape
    {
        /// @brief Boundary behaviour of the x axis.
        FlowWrap WrapX = FlowWrap::Periodic;

        /// @brief Boundary behaviour of the y axis.
        FlowWrap WrapY = FlowWrap::Clamped;

        /// @brief How far one advect step moves the dye, per unit of velocity.
        ///
        /// The advection is unconditionally stable, so this is a quality knob rather than a
        /// stability constraint: a larger scale travels further per dispatch and diffuses more. A
        /// caller tunes advance-per-step by moving this alone.
        f32 StepScale = 1.0f;

        /// @brief An optional per-row scale on the x axis, one entry per grid row.
        ///
        /// A row of metric m advances m grid cells per unit of x velocity — the general form of
        /// "rows are shorter here than there", a stretch on the grid that says nothing about what
        /// the grid represents. Empty leaves every row at 1.
        vector<f32> RowMetric;
    };
}
