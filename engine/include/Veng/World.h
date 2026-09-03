#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/ResidencyBatch.h>
#include <Veng/Scene/SimClock.h>
#include <Veng/WorldInstanceId.h>

namespace Veng
{
    class Scene;

    /// @brief A first-class simulated world: a scene, its own tick clock, and its pause state.
    ///
    /// The bundle a WorldRunner owns and drives. It is view-agnostic and transport-agnostic: it
    /// holds no viewport, no seat, and no NetRole — a world does not know it is being presented or
    /// replicated, so the presentation and transport layers point inward at it by handle rather than
    /// the world pointing out at them. The runner owns the scene (holds the Unique); the client-join
    /// seam may replace it (WorldRunner::InstallScene), and GetScene resolves the live one. Pause is
    /// a refcount plus an explicit toggle, so stacked holds and a game toggle compose (IsPaused is
    /// true while either is set).
    struct World
    {
        /// @brief This world's minted identity.
        WorldInstanceId Id;

        /// @brief The runner-owned scene (with its SceneSimulation attached).
        Unique<Scene> OwnedScene;

        /// @brief The live scene this world drives (the owned one; InstallScene may replace it).
        Scene* LiveScene = nullptr;

        /// @brief The fixed-timestep accumulator advancing this world's Sim phase at its own rate.
        SimClock Clock;

        /// @brief The world spawn's not-yet-resident assets, held until the world starts.
        ResidencyBatch Pending;

        /// @brief This frame's interpolation fraction from the last Sim step, for the View push.
        f32 LastAlpha = 0.0f;

        /// @brief Number of held PauseScopes; the world is paused while this is non-zero.
        u32 PauseRefs = 0;

        /// @brief The explicit pause toggle (SetWorldPaused), composed with the refcount.
        bool ExplicitPaused = false;

        /// @brief Returns the live scene this world drives.
        [[nodiscard]] Scene& GetScene() const { return *LiveScene; }

        /// @brief Returns whether this world is paused (a held scope or the explicit toggle).
        [[nodiscard]] bool IsPaused() const { return ExplicitPaused || PauseRefs > 0; }
    };
}
