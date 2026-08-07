#pragma once

#include <Veng/Veng.h>
#include <Veng/Result.h>
#include <Veng/Renderer/FlowField.h>
#include <Veng/Renderer/Types.h>

/// @brief The device-free half of a flow instance's configuration check.
///
/// FlowField::Create resolves its FlowFieldInfo into one of these — presence, extent and format per
/// field, plus the step scale and metric length — and asserts on a rejection, so the rule set itself
/// needs no images, no context and no ICD. Renderer-internal, beside the sources that consume it.
namespace Veng::Renderer
{
    /// @brief One caller-supplied field as the validator sees it.
    struct FlowFieldImageShape
    {
        /// @brief Whether the caller supplied the field at all.
        bool Present = false;
        /// @brief The field's extent in texels.
        uvec2 Extent{0, 0};
        /// @brief The field's texel format.
        Renderer::Format Format = Format::Undefined;
    };

    /// @brief A whole flow configuration's shapes and scalar knobs, free of device resources.
    struct FlowFieldConfig
    {
        /// @brief The velocity field, whose extent is the grid.
        FlowFieldImageShape Velocity;
        /// @brief The dye fields, in the order they were supplied.
        vector<FlowFieldImageShape> Dyes;
        /// @brief Entries in the per-row metric; 0 means none was supplied.
        usize RowMetricCount = 0;
        /// @brief How far one advect step moves the dye, per unit of velocity.
        f32 StepScale = 1.0f;
    };

    /// @brief Checks a configuration's shapes against what the instance's kernels can bind.
    ///
    /// The rejections, in the order they are tested: a missing velocity field, a zero-size grid, a
    /// velocity format the advect kernel cannot read, no dye at all, too many dyes, a missing or
    /// mismatched dye, a dye in a format the store family cannot write, a row metric that is neither
    /// empty nor one entry per row, and a non-positive or non-finite step scale.
    /// @param config The configuration's shapes and knobs.
    /// @return An error naming the first rejection, or success.
    [[nodiscard]] VoidResult ValidateFlowFieldConfig(const FlowFieldConfig& config);
}
