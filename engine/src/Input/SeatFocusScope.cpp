#include <Veng/Input/SeatFocusScope.h>

#include <Veng/Renderer/Viewport.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/InputMappingSystem.h>
#include <Veng/Scene/Scene.h>

#include <utility>

namespace Veng
{
    InputContextStack* InputSeat::ResolveContexts() const
    {
        if (World == nullptr || Viewer == Entity::Null || !World->IsAlive(Viewer))
        {
            return nullptr;
        }
        return World->TryGet<InputContextStack>(Viewer);
    }

    InputSeat ResolveInputSeat(Scene* scene)
    {
        if (scene == nullptr)
        {
            return InputSeat{};
        }

        InputSeat resolved;
        scene->Each<Viewer, InputContextStack, PlayerInput>(
            [&](const Entity seat, Viewer&, InputContextStack&, PlayerInput&)
            {
                if (resolved.Viewer != Entity::Null || !IsLocallyOwned(*scene, seat))
                {
                    return;
                }
                resolved.Viewer = seat;
                resolved.World = scene;
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
        // still resolve actions against them. The contexts are re-resolved from the scene here so a
        // seat held across a structural change swaps the live pool, not a stale pointer.
        if (context.Id().IsValid())
        {
            if (InputContextStack* contexts = m_Seat.ResolveContexts())
            {
                m_SavedContexts = std::move(contexts->Active);
                contexts->Active = {std::move(context)};
                m_SwappedContext = true;
            }
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

        // Restore through a fresh resolve: a structural change while the scope was open moved the
        // pool, and a destroyed seat entity resolves to nullptr — a correct no-op.
        if (m_SwappedContext)
        {
            if (InputContextStack* contexts = m_Seat.ResolveContexts())
            {
                contexts->Active = std::move(m_SavedContexts);
            }
        }

        m_Router.PopFocus(m_Token);
    }
}
