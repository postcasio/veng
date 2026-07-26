#include <Veng/Scene/InputMappingSystem.h>

#include <Veng/Asset/InputMappingContext.h>
#include <Veng/Input/RawInput.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

namespace Veng
{
    bool IsLocallyOwned(const Scene& scene, Entity seat)
    {
        // A joining client publishes a LocalSeat marker on its own seat. Once any seat carries it,
        // exactly the marked seat is locally owned; the peers' replicated seats are not.
        if (scene.TryGetFirst<LocalSeat>() != nullptr)
        {
            return scene.Has<LocalSeat>(seat);
        }

        // No marker published, so a host answers from Authority: a seat a remote connection owns
        // (Owner != 0) belongs to that peer, not this one. Authority does not replicate, so this
        // branch never fires on a joining client — the marker path above answers there.
        if (const auto* authority = scene.TryGet<Authority>(seat);
            authority != nullptr && authority->Owner != 0)
        {
            return false;
        }

        // Nothing published and no remote owner — single-player, headless, or a host's own seat — so
        // the seat resolves locally, the pre-replication default every single-seat scene keeps.
        return true;
    }

    void InputMappingSystem::OnUpdate(Scene& scene, const f32, const SystemContext& context)
    {
        // A reconciliation replay feeds each tick its recorded PlayerInput directly, so re-resolving
        // from present device state would overwrite the recorded input with the wrong tick's — the
        // one device-reading system a replay must skip.
        if (context.IsReplay)
        {
            return;
        }

        // Reused across seats to gather each stack's resident contexts, lowest priority first.
        vector<ResolvedContext> active;
        scene.Each<Viewer, InputContextStack, PlayerInput, SeatInput>(
            [&](const Entity seat, Viewer&, InputContextStack& stack, PlayerInput& input,
                SeatInput& devices)
            {
                if (!IsLocallyOwned(scene, seat))
                {
                    return;
                }

                // A not-yet-resident context contributes no actions until it streams in — the
                // ordinary async-load contract; skip it and resolve the rest. A focus-gated context
                // is excluded while the seat is not gameplay-focused (IsContextActiveUnderFocus) —
                // pure evaluation over the authored stack, which stays untouched, so a HUD owning the
                // cursor silences gameplay bindings with no stack surgery and no order change.
                active.clear();
                for (const AssetHandle<InputMappingContext>& handle : stack.Active)
                {
                    if (handle.IsLoaded() && IsContextActiveUnderFocus(handle.Get()->GetResolved(),
                                                                       context.GameplayFocused))
                    {
                        active.push_back(handle.Get()->GetResolved());
                    }
                }

                // Each seat resolves against a view scoped to its own devices and the shared
                // pointer routing, so two seats with different assignments produce distinct
                // PlayerInputs and only the pointer's current owner reads the mouse. Phase comes
                // from last tick's resolved state, so the system holds no cross-tick state of its own.
                const SeatInputView raw{context.Input, devices, context.Pointer, seat};
                ActionState resolved = ResolveActions(active, raw, input.State);

                // Accumulate the per-tick action edges across the frame's Sim steps: on any step
                // but the first, OR the prior state's frame-accumulated edges into this step's, so a
                // Started/Completed pulse on a non-final step of a multi-step frame survives to the
                // single View pass that reads it (which the single-valued Phase would overwrite). The
                // first step of the frame keeps only its own edges, starting the accumulation fresh.
                if (!context.FirstStepThisFrame)
                {
                    for (ActionSample& sample : resolved.Actions)
                    {
                        for (const ActionSample& prior : input.State.Actions)
                        {
                            if (prior.Id == sample.Id)
                            {
                                sample.StartedThisFrame |= prior.StartedThisFrame;
                                sample.ReleasedThisFrame |= prior.ReleasedThisFrame;
                                break;
                            }
                        }
                    }
                }
                input.State = std::move(resolved);
            });
    }
}
