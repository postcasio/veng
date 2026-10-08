#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/WorldRunner.h>

#include "RequestDrain.h"

// Scene/PauseRequestReconcile.h — the engine-internal reconcile behind the PauseRequest drain.
//
// A PauseRequest carries whether its world should be paused; the engine holds a single
// request-driven WorldPauseScope per world on behalf of the stamping system (which cannot hold a
// scope across frames) and reconciles the request against it idempotently. Factored out of
// Application so it is device-free-testable: a unit test drives it over a bare WorldRunner.

namespace Veng
{
    struct PauseRequest;

    /// @brief The request-driven pauses the engine holds for PauseRequest stampers, keyed by world id.
    ///
    /// At most one per world, and only a held scope is ever stored.
    using PauseRequestScopes = unordered_map<u64, WorldPauseScope>;

    /// @brief Reconciles one PauseRequest against the engine-held pause of the world it was stamped in.
    ///
    /// A Client-tier world fails (its simulation time is the server's). Otherwise a Paused request
    /// with no request pause held on the world opens a WorldPauseScope and keeps it, an unpaused
    /// request with one held drops it, and requesting the state already held is a no-op. A pause
    /// that cannot be held — the world has no simulation — is handled as a no-op, and nothing is
    /// stored. Only the scope this seam opened is ever dropped, so an overlay's scope or the explicit
    /// toggle is never released by a request.
    /// @param runner   The runner the world is open in.
    /// @param scopes   The engine-owned held pauses, updated in place.
    /// @param world    The world the request was stamped in, and the one it pauses.
    /// @param role     The world's net role.
    /// @param request  The request to reconcile.
    /// @param error    Filled with the reason on failure.
    /// @return RequestResult::Failed on a Client-tier world, RequestResult::Handled otherwise.
    RequestResult ReconcilePauseRequest(WorldRunner& runner, PauseRequestScopes& scopes,
                                        WorldInstanceId world, NetRole role,
                                        const PauseRequest& request, string& error);

    /// @brief Drops a closed world's request-driven pause, if one is held.
    ///
    /// The runner's world-closed hook body. The world is already gone, so the dropped scope's release
    /// resolves nothing and touches no simulation.
    /// @param scopes  The engine-owned held pauses, updated in place.
    /// @param world   The world that closed.
    void ForgetWorldPause(PauseRequestScopes& scopes, WorldInstanceId world);
}
