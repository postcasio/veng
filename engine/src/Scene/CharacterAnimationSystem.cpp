#include <Veng/Scene/CharacterAnimationSystem.h>

#include <Veng/Physics/CharacterController.h>
#include <Veng/Scene/AnimationBlend.h>
#include <Veng/Scene/Scene.h>

namespace Veng
{
    void CharacterAnimationSystem::OnUpdate(Scene& scene, const f32 /*delta*/,
                                            const SystemContext& /*context*/)
    {
        for (auto [entity, state] : scene.View<CharacterState>())
        {
            if (auto* blend = scene.TryGet<AnimationBlend>(entity))
            {
                blend->Parameter = state.PlanarSpeed;
            }
            if (auto* stateSet = scene.TryGet<AnimationStateSet>(entity))
            {
                stateSet->RequestedState = state.Grounded ? string() : CharacterAirborneState;
            }
        }
    }
}
