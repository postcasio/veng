#pragma once

#include <Veng/Veng.h>
#include <Veng/Result.h>
#include <Veng/Renderer/FluidSim.h>
#include <Veng/Renderer/Types.h>

/// @brief The device-free half of a fluid solver's configuration check.
///
/// FluidSim::Create resolves its FluidSimInfo into one of these — presence, extent and format
/// per field, plus the scalar knobs — and asserts on a rejection, so the rule set itself needs
/// no images, no context and no ICD. Renderer-internal, beside the sources that consume it.
namespace Veng::Renderer
{
    /// @brief One caller-supplied field as the validator sees it.
    struct FluidFieldShape
    {
        /// @brief Whether the caller supplied the field at all.
        bool Present = false;
        /// @brief The field's extent in texels.
        uvec2 Extent{0, 0};
        /// @brief The field's texel format.
        Renderer::Format Format = Format::Undefined;
    };

    /// @brief A whole fluid configuration's shapes and scalar knobs, free of device resources.
    struct FluidSimShape
    {
        /// @brief The velocity field, whose extent is the grid.
        FluidFieldShape Velocity;
        /// @brief The dye fields, in the order they were supplied.
        vector<FluidFieldShape> Dyes;
        /// @brief The optional relaxation target.
        FluidFieldShape RelaxationTarget;
        /// @brief The optional damping mask.
        FluidFieldShape DampingMask;
        /// @brief Entries in the per-row metric; 0 means none was supplied.
        usize RowMetricCount = 0;
        /// @brief Jacobi iterations per projection.
        u32 JacobiIterations = DefaultFluidJacobiIterations;
        /// @brief How much simulated time one step advances.
        f32 TimeStep = 1.0f;
    };

    /// @brief Checks a configuration's shapes against what the solver's kernels can bind.
    ///
    /// The rejections, in the order they are tested: a missing velocity field, a zero-size grid,
    /// a velocity format no force/gradient variant writes, too many dyes, a missing or
    /// mismatched dye, an optional field at the wrong extent or in an unwritable format, a row
    /// metric that is neither empty nor one entry per row, a projection with no iterations, and
    /// a non-positive or non-finite timestep.
    /// @param shape The configuration's shapes and knobs.
    /// @return An error naming the first rejection, or success.
    [[nodiscard]] VoidResult ValidateFluidSimShape(const FluidSimShape& shape);
}
