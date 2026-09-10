#pragma once

#include <Veng/Veng.h>

#include <algorithm>
#include <span>

/// @brief Device-free resolve of a scene's PostProcessEffect components into an ordered run.
///
/// The renderer gathers the scene's PostProcessEffect components each Execute and runs the active
/// ones fullscreen in the HDR tail. The decision — which effects are active, in what order, and
/// whether the set changed since the last frame — is a pure function of each component's facts, so
/// it lives here as device-free logic pinned by unit cases (the FrameTopology precedent) rather than
/// only through a rendered image.
namespace Veng::Renderer
{
    /// @brief One scene PostProcessEffect reduced to the facts the resolve reads.
    struct PostProcessEffectInput
    {
        /// @brief The component's Order sort key (effects run ascending).
        i32 Order = 0;
        /// @brief Whether the component is enabled.
        bool Enabled = true;
        /// @brief Whether the component's material handle is resident.
        bool MaterialLoaded = false;
        /// @brief The component's material asset id, the identity the set-change compare keys on.
        u64 MaterialId = 0;
    };

    /// @brief One resolved active effect in run order.
    struct PostProcessEffectEntry
    {
        /// @brief Index of the source component in the resolve input (maps back to its handle).
        usize SourceIndex = 0;
        /// @brief The effect's material asset id (part of the set-change signature).
        u64 MaterialId = 0;
        /// @brief The effect's Order (part of the set-change signature).
        i32 Order = 0;
    };

    /// @brief Filters to enabled, loaded effects and stable-sorts them by ascending Order.
    ///
    /// A disabled effect or one whose material is not resident is dropped; ties on Order keep input
    /// (scene iteration) order, so the run is a total order that does not move frame to frame.
    /// @param inputs  One entry per scene PostProcessEffect component, in scene iteration order.
    /// @return The active effects in run order.
    inline vector<PostProcessEffectEntry>
    ResolveActivePostProcessEffects(std::span<const PostProcessEffectInput> inputs)
    {
        vector<PostProcessEffectEntry> active;
        for (usize i = 0; i < inputs.size(); ++i)
        {
            const PostProcessEffectInput& in = inputs[i];
            if (in.Enabled && in.MaterialLoaded)
            {
                active.push_back(
                    {.SourceIndex = i, .MaterialId = in.MaterialId, .Order = in.Order});
            }
        }
        std::ranges::stable_sort(
            active, [](const PostProcessEffectEntry& a, const PostProcessEffectEntry& b)
            { return a.Order < b.Order; });
        return active;
    }
}
