#pragma once

#include <Veng/Veng.h>
#include <Veng/InputRouter.h>
#include <Veng/Scene/Entity.h>

#include "RequestDrain.h"

// Scene/FocusRequestReconcile.h — the engine-internal reconcile behind the FocusRequest drain.
//
// A FocusRequest carries the input focus a seat should hold; the engine owns a single per-seat
// request-driven FocusToken on behalf of the stamping system (which cannot hold a token across
// frames) and reconciles the request against it idempotently. Factored out of Application so it is
// device-free-testable: a unit test drives it over a headless InputRouter and a bare token map.

namespace Veng
{
    struct FocusRequest;

    /// @brief The per-seat request-driven focus tokens the engine holds for FocusRequest stampers.
    ///
    /// One token per affected seat, keyed by the resolved seat entity (Entity::Null resolved to the
    /// cursor seat before keying). Present means the engine is holding gameplay focus for that seat
    /// on a system's behalf; absent means it is not.
    using FocusRequestTokens = unordered_map<Entity, FocusToken>;

    /// @brief Reconciles one FocusRequest against the router, owning a single per-seat token.
    ///
    /// Resolves the request's Seat (Entity::Null → the router's cursor seat), then: a Gameplay
    /// request with no engine-held token pushes gameplay focus and stores the token; a UI request
    /// with a token held pops that exact token and clears it; requesting the state already held is a
    /// no-op. The engine only ever pops the token it itself pushed, so an interleaved overlay /
    /// SeatFocusScope token is never disturbed. Always succeeds — there is no failure path — so it
    /// returns RequestResult::Handled and never fills @p error.
    /// @param router   The router whose per-seat focus stack is reconciled.
    /// @param tokens   The engine-owned per-seat token map, updated in place.
    /// @param request  The request to reconcile.
    /// @param error    Unused; the reconcile has no failure path.
    /// @return RequestResult::Handled always.
    RequestResult ReconcileFocusRequest(InputRouter& router, FocusRequestTokens& tokens,
                                        const FocusRequest& request, string& error);
}
