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
// device-free-testable: a unit test drives it over a headless InputRouter and a bare token list.

namespace Veng
{
    struct FocusRequest;

    /// @brief The request-driven focus tokens the engine holds for FocusRequest stampers.
    ///
    /// Every token the seam has pushed and not yet popped, not keyed by seat: a held entry moves with
    /// the cursor (InputRouter::MoveCursorSeat), so the seam finds which of its tokens a seat holds by
    /// asking the router (InputRouter::IsFocusTokenOn) rather than remembering where it pushed.
    using FocusRequestTokens = vector<FocusToken>;

    /// @brief Reconciles one FocusRequest against the router, holding at most one token per seat.
    ///
    /// Resolves the request's Seat in the requesting world (Entity::Null → the router's cursor
    /// seat), then: a Gameplay request with no engine-held token on that seat pushes gameplay focus
    /// and keeps the token; a UI request with one held pops that exact token and forgets it;
    /// requesting the state already held is a no-op. The engine only ever pops a token it itself
    /// pushed, so an interleaved overlay / SeatFocusScope token is never disturbed. Tokens popped
    /// by another path (InputRouter::PopFocus() on the cursor seat) are forgotten on the next
    /// reconcile; a token whose entry a window-focus loss suspended is still held. Always succeeds —
    /// there is no failure path — so it returns RequestResult::Handled and never fills @p error.
    /// @param router   The router whose per-seat focus stack is reconciled.
    /// @param tokens   The engine-owned held tokens, updated in place.
    /// @param world    The world the request was stamped in, which its Seat entity belongs to.
    /// @param request  The request to reconcile.
    /// @param error    Unused; the reconcile has no failure path.
    /// @return RequestResult::Handled always.
    RequestResult ReconcileFocusRequest(InputRouter& router, FocusRequestTokens& tokens,
                                        WorldInstanceId world, const FocusRequest& request,
                                        string& error);
}
