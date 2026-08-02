#include "FluidSimShape.h"

#include <cmath>

#include <fmt/format.h>

namespace Veng::Renderer
{
    namespace
    {
        bool IsVelocityFormat(const Format format)
        {
            return format == Format::RG16Sfloat || format == Format::RG32Sfloat;
        }

        bool IsDyeFormat(const Format format)
        {
            return format == Format::R16Sfloat || format == Format::RG16Sfloat ||
                   format == Format::RGBA16Sfloat;
        }

        bool IsMaskFormat(const Format format)
        {
            return format == Format::R16Sfloat || format == Format::R32Sfloat;
        }

        string Describe(const Format format)
        {
            return fmt::format("format {}", static_cast<u32>(format));
        }
    }

    VoidResult ValidateFluidSimShape(const FluidSimShape& shape)
    {
        if (!shape.Velocity.Present)
        {
            return std::unexpected("FluidSim: no velocity field was supplied");
        }
        if (shape.Velocity.Extent.x == 0 || shape.Velocity.Extent.y == 0)
        {
            return std::unexpected(fmt::format("FluidSim: the grid is zero-sized ({}x{})",
                                               shape.Velocity.Extent.x, shape.Velocity.Extent.y));
        }
        if (!IsVelocityFormat(shape.Velocity.Format))
        {
            return std::unexpected(
                fmt::format("FluidSim: the velocity field is {}, not RG16Sfloat or RG32Sfloat",
                            Describe(shape.Velocity.Format)));
        }

        const uvec2 grid = shape.Velocity.Extent;

        if (shape.Dyes.size() > MaxFluidDyes)
        {
            return std::unexpected(fmt::format("FluidSim: {} dye fields exceeds the maximum of {}",
                                               shape.Dyes.size(), MaxFluidDyes));
        }
        for (usize i = 0; i < shape.Dyes.size(); ++i)
        {
            const FluidFieldShape& dye = shape.Dyes[i];
            if (!dye.Present)
            {
                return std::unexpected(fmt::format("FluidSim: dye {} has no image", i));
            }
            if (dye.Extent != grid)
            {
                return std::unexpected(
                    fmt::format("FluidSim: dye {} is {}x{}, not the grid's {}x{}", i, dye.Extent.x,
                                dye.Extent.y, grid.x, grid.y));
            }
            if (!IsDyeFormat(dye.Format))
            {
                return std::unexpected(
                    fmt::format("FluidSim: dye {} is {}, not R16Sfloat, RG16Sfloat or RGBA16Sfloat",
                                i, Describe(dye.Format)));
            }
        }

        if (shape.RelaxationTarget.Present)
        {
            if (shape.RelaxationTarget.Extent != grid)
            {
                return std::unexpected(
                    fmt::format("FluidSim: the relaxation target is {}x{}, not the grid's {}x{}",
                                shape.RelaxationTarget.Extent.x, shape.RelaxationTarget.Extent.y,
                                grid.x, grid.y));
            }
            if (!IsVelocityFormat(shape.RelaxationTarget.Format))
            {
                return std::unexpected(fmt::format(
                    "FluidSim: the relaxation target is {}, not RG16Sfloat or RG32Sfloat",
                    Describe(shape.RelaxationTarget.Format)));
            }
        }

        if (shape.DampingMask.Present)
        {
            if (shape.DampingMask.Extent != grid)
            {
                return std::unexpected(fmt::format(
                    "FluidSim: the damping mask is {}x{}, not the grid's {}x{}",
                    shape.DampingMask.Extent.x, shape.DampingMask.Extent.y, grid.x, grid.y));
            }
            if (!IsMaskFormat(shape.DampingMask.Format))
            {
                return std::unexpected(
                    fmt::format("FluidSim: the damping mask is {}, not R16Sfloat or R32Sfloat",
                                Describe(shape.DampingMask.Format)));
            }
        }

        if (shape.RowMetricCount != 0 && shape.RowMetricCount != grid.y)
        {
            return std::unexpected(
                fmt::format("FluidSim: the row metric has {} entries, not one per row ({})",
                            shape.RowMetricCount, grid.y));
        }

        if (shape.JacobiIterations == 0)
        {
            return std::unexpected("FluidSim: the projection needs at least one Jacobi iteration");
        }

        if (!std::isfinite(shape.TimeStep) || shape.TimeStep <= 0.0f)
        {
            return std::unexpected(fmt::format(
                "FluidSim: the timestep {} is not positive and finite", shape.TimeStep));
        }

        return {};
    }
}
