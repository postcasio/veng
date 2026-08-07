#include "FlowFieldConfig.h"

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

        string Describe(const Format format)
        {
            return fmt::format("format {}", static_cast<u32>(format));
        }
    }

    VoidResult ValidateFlowFieldConfig(const FlowFieldConfig& config)
    {
        if (!config.Velocity.Present)
        {
            return std::unexpected("FlowField: no velocity field was supplied");
        }
        if (config.Velocity.Extent.x == 0 || config.Velocity.Extent.y == 0)
        {
            return std::unexpected(fmt::format("FlowField: the grid is zero-sized ({}x{})",
                                               config.Velocity.Extent.x, config.Velocity.Extent.y));
        }
        if (!IsVelocityFormat(config.Velocity.Format))
        {
            return std::unexpected(
                fmt::format("FlowField: the velocity field is {}, not RG16Sfloat or RG32Sfloat",
                            Describe(config.Velocity.Format)));
        }

        const uvec2 grid = config.Velocity.Extent;

        if (config.Dyes.empty())
        {
            return std::unexpected("FlowField: no dye field was supplied");
        }
        if (config.Dyes.size() > MaxFlowDyes)
        {
            return std::unexpected(fmt::format("FlowField: {} dye fields exceeds the maximum of {}",
                                               config.Dyes.size(), MaxFlowDyes));
        }
        for (usize i = 0; i < config.Dyes.size(); ++i)
        {
            const FlowFieldImageShape& dye = config.Dyes[i];
            if (!dye.Present)
            {
                return std::unexpected(fmt::format("FlowField: dye {} has no image", i));
            }
            if (dye.Extent != grid)
            {
                return std::unexpected(
                    fmt::format("FlowField: dye {} is {}x{}, not the grid's {}x{}", i, dye.Extent.x,
                                dye.Extent.y, grid.x, grid.y));
            }
            if (!IsDyeFormat(dye.Format))
            {
                return std::unexpected(fmt::format(
                    "FlowField: dye {} is {}, not R16Sfloat, RG16Sfloat or RGBA16Sfloat", i,
                    Describe(dye.Format)));
            }
        }

        if (config.RowMetricCount != 0 && config.RowMetricCount != grid.y)
        {
            return std::unexpected(
                fmt::format("FlowField: the row metric has {} entries, not one per row ({})",
                            config.RowMetricCount, grid.y));
        }

        if (!std::isfinite(config.StepScale) || config.StepScale <= 0.0f)
        {
            return std::unexpected(fmt::format(
                "FlowField: the step scale {} is not positive and finite", config.StepScale));
        }

        return {};
    }
}
