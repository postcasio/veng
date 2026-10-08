#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/ResidencyBatch.h>
#include <Veng/Diagnostics/TraceSink.h>
#include <Veng/Scene/SimClock.h>
#include <Veng/WorldInstanceId.h>

namespace Veng
{
    class Scene;

    /// @brief A first-class simulated world: a scene and its own tick clock.
    ///
    /// The bundle a WorldRunner owns and drives. It is view-agnostic and transport-agnostic: it
    /// holds no viewport, no seat, and no NetRole — a world does not know it is being presented or
    /// replicated, so the presentation and transport layers point inward at it by handle rather than
    /// the world pointing out at them. The runner owns the scene (holds the Unique); the client-join
    /// seam may replace it (WorldRunner::InstallScene), and GetScene resolves the live one. The
    /// world's pause lives on its live scene's SceneSimulation, where the scene can see it.
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

        /// @brief The profiler name of this world's Sim phase scope, interned when the world opens.
        ///
        /// Interned once so the per-frame scope formats and hashes nothing; 0 (no name) when no
        /// profiler was installed at open.
        Diagnostics::NameId SimScopeName = 0;

        /// @brief The profiler name of this world's View phase scope, interned when the world opens.
        Diagnostics::NameId ViewScopeName = 0;

        /// @brief Returns the live scene this world drives.
        [[nodiscard]] Scene& GetScene() const { return *LiveScene; }

        /// @brief Returns whether this world's live simulation is paused; false when it has none.
        ///
        /// Forwards to Scene::IsSimulationPaused on the live scene.
        [[nodiscard]] bool IsPaused() const;
    };
}
