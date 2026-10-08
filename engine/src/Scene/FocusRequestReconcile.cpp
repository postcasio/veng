#include "FocusRequestReconcile.h"

#include <Veng/Scene/Requests.h>

#include <algorithm>

namespace Veng
{
    namespace
    {
        // Forgets every held token the router no longer holds an entry for. A retired token (its
        // world closed) is popped, which is how the router forgets it; any other dead token was popped
        // out from under the seam — an anonymous PopFocus() on the cursor seat — and kept, would be
        // popped by a later release: a fatal mispaired pop.
        void DropDeadTokens(InputRouter& router, FocusRequestTokens& tokens)
        {
            std::erase_if(tokens,
                          [&router](const FocusToken token)
                          {
                              if (router.IsFocusTokenLive(token))
                              {
                                  return false;
                              }
                              if (router.IsFocusTokenRetired(token))
                              {
                                  router.PopFocus(token);
                              }
                              return true;
                          });
        }
    }

    RequestResult ReconcileFocusRequest(InputRouter& router, FocusRequestTokens& tokens,
                                        const WorldInstanceId world, const FocusRequest& request,
                                        string&)
    {
        // Entity::Null names the cursor seat — the single keyboard/mouse seat whose focus drives the
        // OS cursor capture — the same convenience PushFocus(InputFocus) resolves to.
        const SeatRef seat = request.Seat.IsNull()
                                 ? router.GetCursorSeat()
                                 : SeatRef{.World = world, .Viewer = request.Seat};

        DropDeadTokens(router, tokens);
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

    void ForgetWorldFocus(InputRouter& router, FocusRequestTokens& tokens,
                          const WorldInstanceId world)
    {
        router.ForgetWorld(world);
        DropDeadTokens(router, tokens);
    }
}
