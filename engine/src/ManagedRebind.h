#pragma once

#include <Veng/Veng.h>
#include <Veng/ManagedViewports.h>
#include <Veng/World.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    class AssetManager;
    class Scene;
    class WorldRunner;

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

    /// @brief Returns whether a world is presentable to both the engine and a consumer's gate.
    ///
    /// The composed present-on-ready test: the engine's own readiness above, and — only once that
    /// passes — @p gate's answer for the resolved world, so a consumer holds the swap for per-world
    /// work of its own. An empty gate reduces this to IsWorldPresentable exactly.
    /// @param runner  The runner the world resolves through.
    /// @param world   The world to test.
    /// @param gate    The consumer predicate, or an empty function for the engine's test alone.
    /// @return True once both tests pass.
    [[nodiscard]] bool IsWorldPresentable(const WorldRunner& runner, WorldInstanceId world,
                                          const WorldPresentReadyGate& gate);

    /// @brief Instantiates the documents of a waiting world's visible overlays, without blocking.
    ///
    /// Run each frame a present-on-ready rebind waits on @p scene's world, so the frame that swaps it
    /// in does not also load and instantiate its overlays' documents (GuiOverlay::Prepare). A hidden
    /// overlay is skipped: it draws nothing when presented, so nothing waits on it.
    /// @param scene   The destination scene whose overlays are prepared.
    /// @param assets  The asset manager the documents load through.
    /// @return True once every visible overlay's document is live, has failed, or names none.
    [[nodiscard]] bool PrepareWorldOverlays(const Scene& scene, AssetManager& assets);
}
