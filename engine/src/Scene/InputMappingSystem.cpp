#include <Veng/Scene/InputMappingSystem.h>

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
        const RawInput raw{context.Input};
        scene.Each<Viewer, InputContextStack, PlayerInput>(
            [&](const Entity seat, Viewer&, InputContextStack& stack, PlayerInput& input)
            {
                if (!IsLocallyOwned(scene, seat))
                {
                    return;
                }

                // Phase comes from last tick's resolved state, so the system holds no
                // cross-tick state of its own.
                input.State = ResolveActions(stack.Active, raw, input.State);
            });
    }
}
