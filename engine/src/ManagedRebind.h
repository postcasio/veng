#pragma once

#include <Veng/Veng.h>
#include <Veng/World.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    class Scene;
    class WorldRunner;

    /// @brief Resolves the seat a viewport re-points to when it is rebound to a destination scene.
    ///
    /// The seat-selection a world rebind applies, mirroring the claiming rules a GuiOverlay already
    /// uses: the currently bound seat when it still resolves as a live Viewer in the destination scene
    /// (entity handles are scene-local, so a handle carried from the departed scene usually does not),
    /// else the scene's sole/first Viewer entity, else null when the scene seats no viewer. A null
    /// result clears the viewport's seat association.
    /// @param scene        The destination scene the seat is resolved in.
    /// @param boundViewer  The seat the viewport was bound to before the rebind (Entity::Null if none).
    /// @return The resolved destination seat, or Entity::Null when the scene seats none.
    [[nodiscard]] Entity ResolvePresentationSeat(const Scene& scene, Entity boundViewer);

    /// @brief Returns whether a world is ready to be presented by a present-on-ready rebind.
    ///
    /// A world is presentable once it resolves through @p runner, its live scene is installed, its
    /// simulation has started, its spawn residency batch reports resident, and its clock has completed
    /// at least one tick — the point past which a rebind onto it shows real content rather than an empty
    /// or half-loaded frame. An unresolved (unminted or closed) world is never presentable.
    /// @param runner  The runner the world resolves through.
    /// @param world   The world to test.
    /// @return True once the world is fully ready to present.
    [[nodiscard]] bool IsWorldPresentable(const WorldRunner& runner, WorldInstanceId world);
}
