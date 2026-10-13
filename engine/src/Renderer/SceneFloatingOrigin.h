#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

namespace Veng::Renderer
{
    /// @brief The scene's FloatingOrigin, or a default one for a fixed-origin scene.
    ///
    /// The first walked wins when the scene carries several; CountFloatingOrigins says whether it
    /// does. Device-free, so the renderer's read is unit-testable.
    /// @param scene  The scene being rendered.
    /// @return The component's value, or a zero Position and a zero ViewerFrameReach.
    [[nodiscard]] inline FloatingOrigin SceneFloatingOrigin(const Scene& scene)
    {
        const auto* origin = scene.TryGetFirst<FloatingOrigin>();
        return origin != nullptr ? *origin : FloatingOrigin{};
    }

    /// @brief How many FloatingOrigin components the scene carries.
    /// @param scene  The scene being rendered.
    [[nodiscard]] inline u32 CountFloatingOrigins(const Scene& scene)
    {
        u32 count = 0;
        for ([[maybe_unused]] auto [entity, component] : scene.View<FloatingOrigin>())
        {
            ++count;
        }
        return count;
    }
}
