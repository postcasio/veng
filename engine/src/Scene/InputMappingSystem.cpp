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
        // No remote ownership exists yet: every seat resolves locally. The net layer maps
        // Authority tier/owner to local ownership here.
        (void)scene;
        (void)seat;
        return true;
    }

    void InputMappingSystem::OnUpdate(Scene& scene, const f32, const SystemContext& context)
    {
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
                // ordinary async-load contract; skip it and resolve the rest.
                active.clear();
                for (const AssetHandle<InputMappingContext>& handle : stack.Active)
                {
                    if (handle.IsLoaded())
                    {
                        active.push_back(handle.Get()->GetResolved());
                    }
                }

                // Each seat resolves against a view scoped to its own devices, so two seats with
                // different assignments produce distinct PlayerInputs. Phase comes from last tick's
                // resolved state, so the system holds no cross-tick state of its own.
                const SeatInputView raw{context.Input, devices};
                input.State = ResolveActions(active, raw, input.State);
            });
    }
}
