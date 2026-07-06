#include <Veng/Input/SeatFocusScope.h>

#include <Veng/Renderer/Viewport.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/InputMappingSystem.h>
#include <Veng/Scene/Scene.h>

#include <utility>

namespace Veng
{
    InputSeat ResolveInputSeat(Scene* scene)
    {
        if (scene == nullptr)
        {
            return InputSeat{};
        }

        InputSeat resolved;
        scene->Each<Viewer, InputContextStack, PlayerInput>(
            [&](const Entity seat, Viewer&, InputContextStack& contexts, PlayerInput&)
            {
                if (resolved.Viewer != Entity::Null || !IsLocallyOwned(*scene, seat))
                {
                    return;
                }
                resolved.Viewer = seat;
                resolved.Contexts = &contexts;
            });
        return resolved;
    }

    SeatFocusScope::SeatFocusScope(InputRouter& router, const InputSeat& seat,
                                   const Renderer::Viewport* viewport,
                                   AssetHandle<InputMappingContext> context)
        : m_Router(router), m_Seat(seat), m_Viewport(viewport)
    {
        // An empty seat makes the scope inert: nothing is flipped, so the destructor restores
        // nothing. A consumer that opens a scope before its world spawns is safe.
        if (m_Seat.Viewer == Entity::Null)
        {
            m_Viewport = nullptr;
            return;
        }

        // (a) Push a token UI entry on the seat's focus stack — the routing owner of the takeover.
        m_Token = m_Router.PushFocus(m_Seat.Viewer, InputFocus::UI);

        // (b) Swap the seat's context stack to the UI context, suspending the gameplay contexts.
        // A caller that supplies no context leaves the gameplay contexts active — a UI screen can
        // still resolve actions against them.
        if (m_Seat.Contexts != nullptr && context.Id().IsValid())
        {
            m_SavedContexts = std::move(m_Seat.Contexts->Active);
            m_Seat.Contexts->Active = {std::move(context)};
            m_SwappedContext = true;
        }

        // (c) Associate the viewport with the seat for pointer routing.
        if (m_Viewport != nullptr)
        {
            m_Router.AssociateViewportSeat(*m_Viewport, m_Seat.Viewer);
        }
    }

    SeatFocusScope::~SeatFocusScope()
    {
        if (m_Seat.Viewer == Entity::Null)
        {
            return;
        }

        // Restore in inverse order: drop the association, restore the context, pop the token.
        if (m_Viewport != nullptr)
        {
            m_Router.ClearViewportSeat(*m_Viewport);
        }

        if (m_SwappedContext && m_Seat.Contexts != nullptr)
        {
            m_Seat.Contexts->Active = std::move(m_SavedContexts);
        }

        m_Router.PopFocus(m_Token);
    }
}
