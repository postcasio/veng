#include "FocusRequestReconcile.h"

#include <Veng/Scene/Requests.h>

namespace Veng
{
    RequestResult ReconcileFocusRequest(InputRouter& router, FocusRequestTokens& tokens,
                                        const FocusRequest& request, string&)
    {
        // Entity::Null names the cursor seat — the single keyboard/mouse seat whose focus drives the
        // OS cursor capture — the same convenience PushFocus(InputFocus) resolves to.
        const Entity seat = request.Seat.IsNull() ? router.GetCursorSeat() : request.Seat;
        auto held = tokens.find(seat);

        // Drop a token the router popped out from under us — window-focus loss (alt-tab) releases the
        // cursor seat's gameplay focus directly, leaving our cached token naming no live entry. Left
        // in place it would make a re-capture a no-op (we would think the seat is still held) and a
        // release pop a dead token (a fatal mispaired pop). Cleared, a re-capture pushes fresh.
        if (held != tokens.end() && !router.IsFocusTokenLive(held->second))
        {
            tokens.erase(held);
            held = tokens.end();
        }
        const bool haveToken = held != tokens.end();

        if (request.Focus == InputFocus::Gameplay)
        {
            // Capture gameplay focus once and keep the token across frames; a second Gameplay
            // request while already held is a no-op success (no extra token, no extra push).
            if (!haveToken)
            {
                tokens.emplace(seat, router.PushFocus(seat, InputFocus::Gameplay));
            }
        }
        else
        {
            // Release only the token this seam itself pushed. PopFocus removes it wherever it sits in
            // the seat's stack, so an overlay / SeatFocusScope token pushed above it is left intact.
            if (haveToken)
            {
                router.PopFocus(held->second);
                tokens.erase(held);
            }
        }

        return RequestResult::Handled;
    }
}
