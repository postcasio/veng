#include "RoleResolver.h"

#include <Veng/Asset/InputMappingContext.h>
#include <Veng/Input.h>
#include <Veng/Input/RawInput.h>
#include <Veng/InputRouter.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/InputMappingSystem.h>
#include <Veng/Scene/Scene.h>
#include <Veng/World.h>
#include <Veng/WorldRunner.h>

#include <algorithm>

namespace Veng
{
    namespace
    {
        /// @brief The first declaration of an action across a seat's contexts, which carries its role.
        const InputAction* FindDeclaration(const std::span<const ResolvedContext> contexts,
                                           const ActionId id)
        {
            for (const ResolvedContext& context : contexts)
            {
                const auto action = std::ranges::find(context.Actions, id, &InputAction::Id);
                if (action != context.Actions.end())
                {
                    return &*action;
                }
            }
            return nullptr;
        }

        /// @brief Whether any context declares an action with a role.
        bool DeclaresRole(const std::span<const ResolvedContext> contexts)
        {
            return std::ranges::any_of(contexts,
                                       [](const ResolvedContext& context)
                                       {
                                           return std::ranges::any_of(
                                               context.Actions, [](const InputAction& action)
                                               { return action.Role != ActionRole::None; });
                                       });
        }

        /// @brief Whether a sample is active (Started or Ongoing).
        bool IsActive(const ActionSample& sample)
        {
            return sample.Phase == ActionPhase::Started || sample.Phase == ActionPhase::Ongoing;
        }
    }

    ModifierKeys ReadModifierKeys(const RawInputView& raw)
    {
        const auto held = [&raw](const Key left, const Key right)
        { return raw.IsKeyDown(static_cast<u32>(left)) || raw.IsKeyDown(static_cast<u32>(right)); };
        return ModifierKeys{.Shift = held(Key::LeftShift, Key::RightShift),
                            .Control = held(Key::LeftControl, Key::RightControl),
                            .Alt = held(Key::LeftAlt, Key::RightAlt),
                            .Super = held(Key::LeftSuper, Key::RightSuper)};
    }

    void RoleResolver::ResolveSeat(const SeatRef seat,
                                   const std::span<const ResolvedContext> contexts,
                                   const usize trackedOnly, const RawInputView& raw,
                                   const f32 delta, vector<RoleFire>& fires)
    {
        SeatState& state = m_Seats[seat];
        state.Seen = true;

        // A seat whose contexts tag no role resolves nothing, but still carries what it held below,
        // so a press held while its role contexts were away is not new when they return.
        ActionState resolved;
        if (DeclaresRole(contexts))
        {
            resolved = ResolveActions(contexts, raw, state.Previous);
        }

        const usize firstFire = fires.size();
        const auto fire = [&fires, firstFire](const ActionRole role, const bool repeat)
        {
            const bool fired =
                std::any_of(fires.begin() + static_cast<std::ptrdiff_t>(firstFire), fires.end(),
                            [role](const RoleFire& existing) { return existing.Role == role; });
            if (!fired)
            {
                fires.emplace_back(RoleFire{.Role = role, .Repeat = repeat});
            }
        };

        const std::span<const ResolvedContext> live = contexts.subspan(trackedOnly);
        vector<RepeatTimer> timers;
        for (const ActionSample& sample : resolved.Actions)
        {
            const InputAction* action = FindDeclaration(live, sample.Id);
            if (action == nullptr || action->Role == ActionRole::None)
            {
                continue;
            }

            if (sample.Phase == ActionPhase::Started)
            {
                fire(action->Role, false);
                if (action->RepeatRate > 0.0f)
                {
                    const f32 first =
                        action->RepeatDelay > 0.0f ? action->RepeatDelay : action->RepeatRate;
                    timers.emplace_back(
                        RepeatTimer{.Action = sample.Id, .Held = 0.0f, .NextRepeat = first});
                }
                continue;
            }

            if (sample.Phase != ActionPhase::Ongoing || action->RepeatRate <= 0.0f)
            {
                continue;
            }

            const auto carried = std::ranges::find(state.Timers, sample.Id, &RepeatTimer::Action);
            if (carried == state.Timers.end())
            {
                continue;
            }
            RepeatTimer timer = *carried;
            timer.Held += delta;
            if (timer.Held >= timer.NextRepeat)
            {
                fire(action->Role, true);
                // One repeat per frame: a frame long enough to owe several pays one and resumes the
                // cadence from now, rather than bursting the debt through the focus order.
                timer.NextRepeat += action->RepeatRate;
                if (timer.NextRepeat <= timer.Held)
                {
                    timer.NextRepeat = timer.Held + action->RepeatRate;
                }
            }
            timers.push_back(timer);
        }
        state.Timers = std::move(timers);

        // An action that left the contexts while held stays held in the carried state, so it reads
        // Ongoing — not a fresh press — if it returns before release.
        for (const ActionSample& prior : state.Previous.Actions)
        {
            if (IsActive(prior) && std::ranges::find(resolved.Actions, prior.Id,
                                                     &ActionSample::Id) == resolved.Actions.end())
            {
                ActionSample held = prior;
                held.Phase = ActionPhase::Ongoing;
                resolved.Actions.push_back(held);
            }
        }
        state.Previous = std::move(resolved);
    }

    void RoleResolver::RetireUnseen()
    {
        std::erase_if(m_Seats, [](const auto& entry) { return !entry.second.Seen; });
        for (auto& [seat, state] : m_Seats)
        {
            state.Seen = false;
        }
    }

    void RoleResolver::Update(const RoleFrameInfo& frame)
    {
        m_Pending.clear();
        const std::span<const Key> claimed = frame.Router.GetClaimedKeys();

        // Queues a seat's navigation presses when the seat holds UI focus; the focus is read before
        // any press dispatches, so a press that changes focus cannot gate another this frame.
        const auto queue =
            [this](const SeatRef seat, const InputFocus focus, const RawInputView& raw)
        {
            if (focus != InputFocus::UI || m_Fires.empty())
            {
                return;
            }
            const ModifierKeys modifiers = ReadModifierKeys(raw);
            for (const RoleFire& fired : m_Fires)
            {
                if (IsNavigationRole(fired.Role))
                {
                    m_Pending.emplace_back(RoleEvent{.Seat = seat,
                                                     .Role = fired.Role,
                                                     .Modifiers = modifiers,
                                                     .Repeat = fired.Repeat});
                }
            }
        };

        for (const Unique<World>& world : frame.Worlds.GetWorlds())
        {
            Scene& scene = world->GetScene();
            const PointerRouting none{};
            const PointerRouting& pointer = &scene == frame.PointerScene ? frame.Pointer : none;
            scene.Each<Viewer, SeatInput>(
                [&](const Entity viewer, Viewer&, SeatInput& devices)
                {
                    if (!IsLocallyOwned(scene, viewer))
                    {
                        return;
                    }
                    const SeatRef seat{.World = world->Id, .Viewer = viewer};
                    const InputFocus focus = frame.Router.GetFocus(seat);
                    const InputContextStack* stack = scene.TryGet<InputContextStack>(viewer);

                    // An exclusive stack still tracks the default beneath it, firing nothing from
                    // it, so a press held through a takeover is not new when the default returns.
                    m_Contexts.clear();
                    usize trackedOnly = 0;
                    if (frame.DefaultUi != nullptr)
                    {
                        m_Contexts.push_back(frame.DefaultUi->GetResolved());
                        trackedOnly = stack != nullptr && stack->Exclusive ? 1 : 0;
                    }
                    if (stack != nullptr)
                    {
                        for (const AssetHandle<InputMappingContext>& handle : stack->Active)
                        {
                            if (handle.IsLoaded() &&
                                IsContextActiveUnderFocus(handle.Get()->GetResolved(),
                                                          focus == InputFocus::Gameplay))
                            {
                                m_Contexts.push_back(handle.Get()->GetResolved());
                            }
                        }
                    }

                    const FrameInputView raw(frame.Snapshot, devices, pointer, viewer, claimed);
                    m_Fires.clear();
                    ResolveSeat(seat, m_Contexts, trackedOnly, raw, frame.Delta, m_Fires);
                    queue(seat, focus, raw);
                });
        }

        // The implicit seat reads every device, so its presses follow the keyboard's routing: the
        // cursor seat's focus, which gates the window events it reads.
        m_Contexts.clear();
        if (frame.DefaultUi != nullptr)
        {
            m_Contexts.push_back(frame.DefaultUi->GetResolved());
        }
        const FrameInputView raw(frame.Snapshot, claimed);
        m_Fires.clear();
        ResolveSeat(SeatRef{}, m_Contexts, 0, raw, frame.Delta, m_Fires);
        queue(SeatRef{}, frame.Router.GetFocus(frame.Router.GetCursorSeat()), raw);

        RetireUnseen();

        for (const RoleEvent& event : m_Pending)
        {
            frame.Router.DispatchRole(event);
        }
    }
}
