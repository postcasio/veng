#include "FocusRequestReconcile.h"

#include <Veng/Scene/Requests.h>

#include <algorithm>

namespace Veng
{
    RequestResult ReconcileFocusRequest(InputRouter& router, FocusRequestTokens& tokens,
                                        const WorldInstanceId world, const FocusRequest& request,
                                        string&)
    {
        // Entity::Null names the cursor seat — the single keyboard/mouse seat whose focus drives the
        // OS cursor capture — the same convenience PushFocus(InputFocus) resolves to.
        const SeatRef seat = request.Seat.IsNull()
                                 ? router.GetCursorSeat()
                                 : SeatRef{.World = world, .Viewer = request.Seat};

        // Forget tokens the router popped out from under us — an anonymous PopFocus() on the cursor
        // seat. Kept, a dead token would be popped by a later release: a fatal mispaired pop.
        std::erase_if(tokens, [&router](const FocusToken token)
                      { return !router.IsFocusTokenLive(token); });
        const auto held = std::ranges::find_if(tokens, [&router, seat](const FocusToken token)
                                               { return router.IsFocusTokenOn(seat, token); });
        const bool haveToken = held != tokens.end();

        if (request.Focus == InputFocus::Gameplay)
        {
            // Capture gameplay focus once and keep the token across frames; a second Gameplay
            // request while already held is a no-op success (no extra token, no extra push).
            if (!haveToken)
            {
                tokens.push_back(router.PushFocus(seat, InputFocus::Gameplay));
            }
        }
        else
        {
            // Release only the token this seam itself pushed. PopFocus removes it wherever it sits in
            // the seat's stack, so an overlay / SeatFocusScope token pushed above it is left intact.
            if (haveToken)
            {
                router.PopFocus(*held);
                tokens.erase(held);
            }
        }

        return RequestResult::Handled;
    }
}
